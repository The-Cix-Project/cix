"use strict";

/*
 * ADR-0339 (#599): the dashboard is told what changed, on GET /v1/events.
 *
 * A state machine and nothing else. It holds the generations the page
 * has seen, compares each frame against them, and says which reads
 * moved -- it builds no URL, touches no DOM and calls no API, so the
 * page decides what a change means (web/app.js) and the quickjs harness
 * can drive every case here with a fake socket and a fake clock
 * (test/test_web_form.c).
 *
 * The rules, each from the ADR:
 *
 *   - A frame is {"epoch": "...", "gen": {"<operationId>": n, ...}}.
 *   - A read changed when its generation is greater than the one held.
 *     A heartbeat names every read at an unchanged generation, so it
 *     changes nothing and costs the page nothing. A lower generation is
 *     a late frame and is ignored.
 *   - A different epoch means the daemon restarted: everything held is
 *     void, so it is cleared and every read in the frame is changed.
 *     The first frame after a connect is the same case, which is how a
 *     reconnect re-reads.
 *   - Close code 4401 is the session ending (expired, logged out,
 *     revoked). Every other close -- a daemon restart is 1006 -- is
 *     the stream being lost: report it, reconnect with backoff, and
 *     never sign anyone out for it.
 *   - Silence for 25 s is a lost stream too: the daemon sends a
 *     heartbeat every 10 s, so 25 s without a frame means it is gone
 *     whatever the socket claims.
 */
const CIX_EVENTS_SILENCE_MS = 25000;
const CIX_EVENTS_BACKOFF_MS = [1000, 2000, 4000, 8000, 15000];

/*
 * opts:
 *   makeSocket()      -> a WebSocket-shaped object (onopen, onmessage,
 *                        onclose, close()), already pointed at the stream
 *   now()             -> milliseconds
 *   setTimer(fn, ms)  -> id;  clearTimer(id)
 *   onChange(ops)     -- reads that moved, by operationId
 *   onLive(live)      -- true on the first frame, false when lost
 *   onSessionEnded()  -- close code 4401
 *   onFrame()         -- optional: every frame, heartbeats included
 */
function createEventStream(opts) {
	const s = {
		epoch: null,
		held: {},
		socket: null,
		live: false,
		stopped: true,
		lastFrameAt: 0,
		attempt: 0,
		reconnectTimer: null,
		watchdogTimer: null,
	};

	function setLive(live) {
		if (s.live !== live) {
			s.live = live;
			opts.onLive(live);
		}
	}

	/* The frame's reads that moved against what is held; holds the new
	 * values. Exposed for the harness. */
	function apply(frame) {
		const gen = frame.gen || {};
		const changed = [];

		if (frame.epoch !== s.epoch) {
			s.epoch = frame.epoch;
			s.held = {};
		}
		for (const op of Object.keys(gen)) {
			const n = gen[op];

			if (s.held[op] === undefined || n > s.held[op]) {
				s.held[op] = n;
				changed.push(op);
			}
		}
		return changed;
	}

	function watchdog() {
		s.watchdogTimer = null;
		if (s.stopped || s.socket === null)
			return;
		if (opts.now() - s.lastFrameAt >= CIX_EVENTS_SILENCE_MS) {
			/* The socket may still look open; it is not live. */
			const sock = s.socket;

			s.socket = null;
			try {
				sock.close();
			} catch (e) {
				/* already gone */
			}
			lost();
			return;
		}
		s.watchdogTimer = opts.setTimer(watchdog, 5000);
	}

	function lost() {
		setLive(false);
		if (s.stopped || s.reconnectTimer !== null)
			return;
		const delay = CIX_EVENTS_BACKOFF_MS[Math.min(s.attempt, CIX_EVENTS_BACKOFF_MS.length - 1)];

		s.attempt++;
		s.reconnectTimer = opts.setTimer(() => {
			s.reconnectTimer = null;
			connect();
		}, delay);
	}

	function connect() {
		let sock;

		if (s.stopped)
			return;
		try {
			sock = opts.makeSocket();
		} catch (e) {
			lost();
			return;
		}
		s.socket = sock;
		s.lastFrameAt = opts.now();
		sock.onmessage = (ev) => {
			let frame;

			if (s.socket !== sock)
				return;
			try {
				frame = JSON.parse(ev.data);
			} catch (e) {
				return;
			}
			s.lastFrameAt = opts.now();
			s.attempt = 0;
			setLive(true);
			if (opts.onFrame)
				opts.onFrame();
			const changed = apply(frame);

			if (changed.length > 0)
				opts.onChange(changed);
		};
		sock.onclose = (ev) => {
			if (s.socket !== sock)
				return;
			s.socket = null;
			if (ev && ev.code === 4401) {
				setLive(false);
				s.stopped = true;
				opts.onSessionEnded();
				return;
			}
			lost();
		};
		if (s.watchdogTimer === null)
			s.watchdogTimer = opts.setTimer(watchdog, 5000);
	}

	return {
		/* Opens the stream; a no-op while it is already running. */
		start() {
			if (!s.stopped)
				return;
			s.stopped = false;
			s.attempt = 0;
			connect();
		},
		/* Closes it for good -- a logout, or a new session about to be
		 * opened in its place. What is held is kept: the next start's
		 * first frame compares against it. */
		stop() {
			s.stopped = true;
			if (s.reconnectTimer !== null)
				opts.clearTimer(s.reconnectTimer);
			if (s.watchdogTimer !== null)
				opts.clearTimer(s.watchdogTimer);
			s.reconnectTimer = null;
			s.watchdogTimer = null;
			if (s.socket !== null) {
				const sock = s.socket;

				s.socket = null;
				try {
					sock.close();
				} catch (e) {
					/* already gone */
				}
			}
			setLive(false);
		},
		/* A hidden tab's timers are throttled to once a minute, so the
		 * watchdog cannot be trusted across one: on becoming visible the
		 * page asks directly. */
		checkSilence() {
			if (!s.stopped && s.socket !== null &&
			    opts.now() - s.lastFrameAt >= CIX_EVENTS_SILENCE_MS) {
				if (s.watchdogTimer !== null)
					opts.clearTimer(s.watchdogTimer);
				s.watchdogTimer = null;
				watchdog();
			}
		},
		isLive() {
			return s.live;
		},
		apply: apply,
	};
}
