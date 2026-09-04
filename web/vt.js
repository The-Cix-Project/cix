/*
 * The Cix terminal -- a real VT, written here (ADR-0243).
 *
 * This replaces the line-buffer renderer ADR-0043 shipped, which had no
 * two-dimensional screen at all: it interpreted \r\n\b\t and SGR colour
 * and structurally discarded every cursor-addressing escape, so any
 * program that draws rather than prints rendered as nonsense. That was
 * an honest boundary while it stood, and it is the boundary this file
 * removes.
 *
 * Written rather than vendored, for the same reason ADR-0043 hand-rolled
 * the WebSocket: no framework (ADR-0010), and every layer of this
 * platform is ours. The shape is the classic one -- a byte-stream parser
 * feeding a cell grid, and a renderer that only touches rows that
 * changed -- because that is what the problem actually is, not because
 * any particular implementation was copied.
 *
 * Interface, deliberately small:
 *
 *   const vt = createVT(element, { onInput, onResize });
 *   vt.feed(string)        -- decoded output from the pty
 *   vt.fit()               -- remeasure and resize to the element
 *   vt.dispose()
 *
 * onInput(string) is called with bytes to send to the pty; onResize
 * (cols, rows) whenever a fit changed the size, which the caller turns
 * into ADR-0242's resize control message. This file knows nothing about
 * WebSockets, the REST API or the dashboard, and nothing outside it
 * knows how a terminal works.
 */

/*
 * One palette, used for both foreground and background.
 *
 * The 16 base colours are exactly the values the old renderer's CSS
 * carried, so ordinary coloured output looks the way it always has.
 * What changed is that backgrounds now use the SAME colours rather than
 * the muted variants the stylesheet used to define separately (#5c2323
 * for red, and so on). Those existed because colour used to appear in
 * occasional short spans where a full-strength background was
 * shouty. A full-screen program paints backgrounds by the row -- status
 * bars, selections, meters -- and rendering a red status bar as maroon
 * is precisely the misreporting the old stylesheet's own comment said a
 * console must never do. Same principle, applied where it now bites.
 */
const VT_PALETTE = (function () {
	const p = [
		"#1c1c1e", "#e35555", "#4fb85a", "#d2b13a",
		"#4d8fdb", "#b56ad1", "#46b8b0", "#d8d8dc",
		"#7a7a7e", "#ff7b7b", "#7bdb85", "#eeda6f",
		"#7fb1f2", "#dd9bef", "#72e0d8", "#ffffff"
	];
	/* xterm's 6x6x6 cube, on its own non-linear level ramp -- not
	 * evenly spaced, and getting that wrong is what makes 256-colour
	 * output look subtly muddy rather than obviously broken. */
	const levels = [0, 95, 135, 175, 215, 255];
	const hex = (n) => n.toString(16).padStart(2, "0");

	for (let r = 0; r < 6; r++)
		for (let g = 0; g < 6; g++)
			for (let b = 0; b < 6; b++)
				p.push("#" + hex(levels[r]) + hex(levels[g]) + hex(levels[b]));
	for (let i = 0; i < 24; i++) {
		const v = 8 + i * 10;

		p.push("#" + hex(v) + hex(v) + hex(v));
	}
	return p;
})();

/* Attribute bits. A bitfield rather than an object per cell: a screen is
 * tens of thousands of cells and they are compared on every render to
 * decide where a style run ends, so cheap equality matters more here
 * than it does anywhere else in this codebase. */
const VT_BOLD = 1;
const VT_DIM = 2;
const VT_ITALIC = 4;
const VT_UNDERLINE = 8;
const VT_BLINK = 16;
const VT_REVERSE = 32;
const VT_HIDDEN = 64;
const VT_STRIKE = 128;

const VT_SCROLLBACK_MAX = 2000;

/*
 * DEC Special Graphics (ESC ( 0). ncurses reaches for this whenever its
 * terminfo entry describes line drawing through ACS rather than UTF-8 --
 * which the `linux` entry does, and which is how a box-drawn interface
 * arrives as a screenful of qqqqx if the mapping is missing. Only the
 * glyphs that actually differ are listed; everything else passes
 * through unchanged.
 */
const VT_DEC_GRAPHICS = {
	"`": "◆", "a": "▒", "b": "␉", "c": "␌",
	"d": "␍", "e": "␊", "f": "°", "g": "±",
	"h": "␤", "i": "␋", "j": "┘", "k": "┐",
	"l": "┌", "m": "└", "n": "┼", "o": "⎺",
	"p": "⎻", "q": "─", "r": "⎼", "s": "⎽",
	"t": "├", "u": "┤", "v": "┴", "w": "┬",
	"x": "│", "y": "≤", "z": "≥", "{": "π",
	"|": "≠", "}": "£", "~": "·"
};

function createVT(hostEl, opts) {
	const onInput = (opts && opts.onInput) || function () {};
	const onResize = (opts && opts.onResize) || function () {};

	let cols = 80;
	let rows = 24;

	/* Two row containers rather than one. Scrollback and the live screen
	 * have different lifetimes -- a row leaving the screen becomes
	 * history and never changes again -- and keeping them apart makes
	 * that transition a single appendChild rather than a rebuild, and
	 * makes hiding history on the alternate screen a single toggle. */
	const scrollbackEl = document.createElement("div");
	const screenEl = document.createElement("div");

	scrollbackEl.className = "vt-scrollback";
	screenEl.className = "vt-screen";
	hostEl.textContent = "";
	hostEl.appendChild(scrollbackEl);
	hostEl.appendChild(screenEl);

	/* --- screen state --- */

	function blankCell() {
		return { c: " ", fg: null, bg: null, at: 0 };
	}

	function blankRow(n) {
		const r = new Array(n);

		for (let i = 0; i < n; i++)
			r[i] = blankCell();
		return r;
	}

	let buf = [];          /* rows x cols cells, the live screen */
	let altBuf = null;     /* the normal screen, parked while alt is active */
	let dirty = [];        /* per screen row: needs a re-render */
	let rowEls = [];       /* per screen row: its own div, reused across renders */

	let cur = { row: 0, col: 0 };
	let saved = null;
	let sgr = { fg: null, bg: null, at: 0 };
	let scrollTop = 0;
	let scrollBot = 23;
	let wrapPending = false;   /* DECAWM's deferred wrap -- see putChar() */
	let tabs = [];

	const mode = {
		autowrap: true,
		origin: false,
		cursorVisible: true,
		appCursor: false,
		insert: false,
		altScreen: false,
		bracketedPaste: false,
		mouse: 0,        /* 0 none, 1000 click, 1002 drag, 1003 any */
		mouseSgr: false
	};

	let charsetG0 = "B";
	let charsetShifted = false;

	function markDirty(r) {
		if (r >= 0 && r < rows)
			dirty[r] = true;
	}

	function markAllDirty() {
		for (let i = 0; i < rows; i++)
			dirty[i] = true;
	}

	function resetTabs() {
		tabs = [];
		for (let i = 0; i < cols; i++)
			tabs.push(i % 8 === 0);
	}

	function initScreen(newCols, newRows) {
		cols = newCols;
		rows = newRows;
		buf = [];
		for (let i = 0; i < rows; i++)
			buf.push(blankRow(cols));
		dirty = new Array(rows).fill(true);
		scrollTop = 0;
		scrollBot = rows - 1;
		cur = { row: 0, col: 0 };
		wrapPending = false;
		resetTabs();
		rebuildRowEls();
	}

	function rebuildRowEls() {
		screenEl.textContent = "";
		rowEls = [];
		for (let i = 0; i < rows; i++) {
			const d = document.createElement("div");

			d.className = "vt-row";
			screenEl.appendChild(d);
			rowEls.push(d);
		}
		markAllDirty();
	}

	/* --- scrolling --- */

	function scrollUp(n) {
		for (let k = 0; k < n; k++) {
			const leaving = buf[scrollTop];

			/* Only a line leaving the TOP of a full-height screen is
			 * history. A line pushed out of a partial scroll region --
			 * what a program does to scroll a pane inside its own
			 * layout -- was never the whole screen and belongs to
			 * nobody's history. Putting those in scrollback is the
			 * classic way a terminal's history fills with fragments of
			 * a status bar. */
			if (!mode.altScreen && scrollTop === 0 && scrollBot === rows - 1)
				pushScrollback(leaving);

			buf.splice(scrollTop, 1);
			buf.splice(scrollBot, 0, blankRow(cols));
		}
		for (let r = scrollTop; r <= scrollBot; r++)
			markDirty(r);
	}

	function scrollDown(n) {
		for (let k = 0; k < n; k++) {
			buf.splice(scrollBot, 1);
			buf.splice(scrollTop, 0, blankRow(cols));
		}
		for (let r = scrollTop; r <= scrollBot; r++)
			markDirty(r);
	}

	function pushScrollback(row) {
		const d = document.createElement("div");

		d.className = "vt-row";
		renderRowInto(d, row, -1);
		scrollbackEl.appendChild(d);
		while (scrollbackEl.childElementCount > VT_SCROLLBACK_MAX)
			scrollbackEl.removeChild(scrollbackEl.firstChild);
	}

	/* --- writing --- */

	function putChar(ch) {
		/*
		 * DECAWM's wrap is deferred, and this is the single most
		 * commonly got-wrong rule in a VT. Writing the last column does
		 * NOT move the cursor off the line; it leaves it on the last
		 * column with a wrap pending, and only the NEXT printable
		 * character actually wraps. Wrapping eagerly puts a spurious
		 * blank line after every exactly-full row, which is what makes
		 * a full-width table or a status bar drawn to the last column
		 * shove everything below it down by one.
		 */
		if (wrapPending && mode.autowrap) {
			cur.col = 0;
			if (cur.row === scrollBot)
				scrollUp(1);
			else if (cur.row < rows - 1)
				cur.row++;
			wrapPending = false;
		}
		if (cur.col >= cols) {
			if (!mode.autowrap)
				cur.col = cols - 1;
			else
				return;
		}

		if (charsetShifted && charsetG0 === "0" && VT_DEC_GRAPHICS[ch] !== undefined)
			ch = VT_DEC_GRAPHICS[ch];

		const row = buf[cur.row];

		if (mode.insert) {
			row.splice(cols - 1, 1);
			row.splice(cur.col, 0, blankCell());
		}
		row[cur.col] = { c: ch, fg: sgr.fg, bg: sgr.bg, at: sgr.at };
		markDirty(cur.row);

		if (cur.col === cols - 1)
			wrapPending = true;
		else
			cur.col++;
	}

	function lineFeed() {
		wrapPending = false;
		if (cur.row === scrollBot)
			scrollUp(1);
		else if (cur.row < rows - 1)
			cur.row++;
	}

	function reverseIndex() {
		wrapPending = false;
		if (cur.row === scrollTop)
			scrollDown(1);
		else if (cur.row > 0)
			cur.row--;
	}

	/* --- cursor addressing --- */

	function originTop() {
		return mode.origin ? scrollTop : 0;
	}

	function originBot() {
		return mode.origin ? scrollBot : rows - 1;
	}

	function moveTo(r, c) {
		cur.row = Math.max(originTop(), Math.min(originBot(), r));
		cur.col = Math.max(0, Math.min(cols - 1, c));
		wrapPending = false;
	}

	/* --- erasing --- */

	function eraseInRow(r, from, to) {
		const row = buf[r];

		for (let i = from; i <= to && i < cols; i++)
			/* Erasing paints the CURRENT background, not the default
			 * one -- that is what lets a program clear to end-of-line
			 * and have the cleared part keep the colour of the bar it
			 * is drawing, instead of punching a default-coloured hole
			 * in it. */
			row[i] = { c: " ", fg: null, bg: sgr.bg, at: 0 };
		markDirty(r);
	}

	function eraseDisplay(param) {
		if (param === 0) {
			eraseInRow(cur.row, cur.col, cols - 1);
			for (let r = cur.row + 1; r < rows; r++)
				eraseInRow(r, 0, cols - 1);
		} else if (param === 1) {
			eraseInRow(cur.row, 0, cur.col);
			for (let r = 0; r < cur.row; r++)
				eraseInRow(r, 0, cols - 1);
		} else if (param === 2 || param === 3) {
			for (let r = 0; r < rows; r++)
				eraseInRow(r, 0, cols - 1);
			if (param === 3) {
				scrollbackEl.textContent = "";
			}
		}
	}

	/* --- SGR --- */

	function applySgr(params) {
		if (params.length === 0)
			params = [0];
		for (let i = 0; i < params.length; i++) {
			const p = params[i];

			if (p === 0) {
				sgr = { fg: null, bg: null, at: 0 };
			} else if (p === 1) { sgr.at |= VT_BOLD; }
			else if (p === 2) { sgr.at |= VT_DIM; }
			else if (p === 3) { sgr.at |= VT_ITALIC; }
			else if (p === 4) { sgr.at |= VT_UNDERLINE; }
			else if (p === 5 || p === 6) { sgr.at |= VT_BLINK; }
			else if (p === 7) { sgr.at |= VT_REVERSE; }
			else if (p === 8) { sgr.at |= VT_HIDDEN; }
			else if (p === 9) { sgr.at |= VT_STRIKE; }
			else if (p === 21 || p === 22) { sgr.at &= ~(VT_BOLD | VT_DIM); }
			else if (p === 23) { sgr.at &= ~VT_ITALIC; }
			else if (p === 24) { sgr.at &= ~VT_UNDERLINE; }
			else if (p === 25) { sgr.at &= ~VT_BLINK; }
			else if (p === 27) { sgr.at &= ~VT_REVERSE; }
			else if (p === 28) { sgr.at &= ~VT_HIDDEN; }
			else if (p === 29) { sgr.at &= ~VT_STRIKE; }
			else if (p >= 30 && p <= 37) { sgr.fg = p - 30; }
			else if (p === 39) { sgr.fg = null; }
			else if (p >= 40 && p <= 47) { sgr.bg = p - 40; }
			else if (p === 49) { sgr.bg = null; }
			else if (p >= 90 && p <= 97) { sgr.fg = p - 90 + 8; }
			else if (p >= 100 && p <= 107) { sgr.bg = p - 100 + 8; }
			else if (p === 38 || p === 48) {
				/* Extended colour, two spellings: ;5;<n> indexes the
				 * 256-colour palette, ;2;<r>;<g>;<b> is direct. Both
				 * consume their own arguments out of this same
				 * parameter list. */
				const target = p === 38 ? "fg" : "bg";

				if (params[i + 1] === 5) {
					sgr[target] = params[i + 2];
					i += 2;
				} else if (params[i + 1] === 2) {
					sgr[target] = {
						r: params[i + 2] | 0,
						g: params[i + 3] | 0,
						b: params[i + 4] | 0
					};
					i += 4;
				}
			}
		}
	}

	/* --- the parser --- */

	let state = "ground";
	let csiParams = "";
	let csiPrefix = "";
	let oscBuf = "";

	function csiNumbers() {
		if (csiParams === "")
			return [];
		return csiParams.split(";").map((s) => (s === "" ? 0 : parseInt(s, 10) | 0));
	}

	function arg(list, i, dflt) {
		const v = list[i];

		return v === undefined || v === 0 ? dflt : v;
	}

	function setMode(list, on) {
		for (const p of list) {
			if (csiPrefix === "?") {
				if (p === 1) mode.appCursor = on;
				else if (p === 6) { mode.origin = on; moveTo(originTop(), 0); }
				else if (p === 7) mode.autowrap = on;
				else if (p === 25) { mode.cursorVisible = on; markDirty(cur.row); }
				else if (p === 1000 || p === 1002 || p === 1003) mode.mouse = on ? p : 0;
				else if (p === 1006) mode.mouseSgr = on;
				else if (p === 2004) mode.bracketedPaste = on;
				else if (p === 47 || p === 1047 || p === 1049) setAltScreen(on, p === 1049);
			} else if (p === 4) {
				mode.insert = on;
			}
		}
	}

	function setAltScreen(on, alsoCursor) {
		if (on === mode.altScreen)
			return;
		if (on) {
			if (alsoCursor)
				saved = { row: cur.row, col: cur.col, sgr: Object.assign({}, sgr) };
			altBuf = buf;
			buf = [];
			for (let i = 0; i < rows; i++)
				buf.push(blankRow(cols));
			mode.altScreen = true;
			/* History belongs to the normal screen. A full-screen
			 * program's own drawing is not history and must not be
			 * scrollable behind it, or leaving the program leaves its
			 * frames stuck above the shell. */
			scrollbackEl.hidden = true;
		} else {
			buf = altBuf || buf;
			altBuf = null;
			mode.altScreen = false;
			scrollbackEl.hidden = false;
			if (alsoCursor && saved !== null) {
				cur.row = Math.min(saved.row, rows - 1);
				cur.col = Math.min(saved.col, cols - 1);
				sgr = saved.sgr;
			}
		}
		scrollTop = 0;
		scrollBot = rows - 1;
		markAllDirty();
	}

	function execCsi(final) {
		const n = csiNumbers();

		switch (final) {
		case "@": {
			const row = buf[cur.row];
			const count = arg(n, 0, 1);

			for (let k = 0; k < count; k++) {
				row.splice(cols - 1, 1);
				row.splice(cur.col, 0, blankCell());
			}
			markDirty(cur.row);
			break;
		}
		case "A": moveTo(cur.row - arg(n, 0, 1), cur.col); break;
		case "B": moveTo(cur.row + arg(n, 0, 1), cur.col); break;
		case "C": moveTo(cur.row, cur.col + arg(n, 0, 1)); break;
		case "D": moveTo(cur.row, cur.col - arg(n, 0, 1)); break;
		case "E": moveTo(cur.row + arg(n, 0, 1), 0); break;
		case "F": moveTo(cur.row - arg(n, 0, 1), 0); break;
		case "G": case "`": moveTo(cur.row, arg(n, 0, 1) - 1); break;
		case "H": case "f":
			moveTo(originTop() + arg(n, 0, 1) - 1, arg(n, 1, 1) - 1);
			break;
		case "I": {
			let count = arg(n, 0, 1);

			while (count-- > 0)
				tabForward();
			break;
		}
		case "J": eraseDisplay(n[0] || 0); break;
		case "K": {
			const p = n[0] || 0;

			if (p === 0) eraseInRow(cur.row, cur.col, cols - 1);
			else if (p === 1) eraseInRow(cur.row, 0, cur.col);
			else eraseInRow(cur.row, 0, cols - 1);
			break;
		}
		case "L": {
			/* Insert lines, but only inside the scroll region and only
			 * when the cursor is in it -- outside, this is a no-op, not
			 * a screen-wide shift. */
			if (cur.row < scrollTop || cur.row > scrollBot)
				break;
			const count = Math.min(arg(n, 0, 1), scrollBot - cur.row + 1);

			for (let k = 0; k < count; k++) {
				buf.splice(scrollBot, 1);
				buf.splice(cur.row, 0, blankRow(cols));
			}
			for (let r = cur.row; r <= scrollBot; r++)
				markDirty(r);
			break;
		}
		case "M": {
			if (cur.row < scrollTop || cur.row > scrollBot)
				break;
			const count = Math.min(arg(n, 0, 1), scrollBot - cur.row + 1);

			for (let k = 0; k < count; k++) {
				buf.splice(cur.row, 1);
				buf.splice(scrollBot, 0, blankRow(cols));
			}
			for (let r = cur.row; r <= scrollBot; r++)
				markDirty(r);
			break;
		}
		case "P": {
			const row = buf[cur.row];
			const count = arg(n, 0, 1);

			for (let k = 0; k < count; k++) {
				row.splice(cur.col, 1);
				row.push(blankCell());
			}
			markDirty(cur.row);
			break;
		}
		case "S": scrollUp(arg(n, 0, 1)); break;
		case "T": scrollDown(arg(n, 0, 1)); break;
		case "X": {
			const count = arg(n, 0, 1);

			eraseInRow(cur.row, cur.col, cur.col + count - 1);
			break;
		}
		case "Z": {
			let count = arg(n, 0, 1);

			while (count-- > 0)
				tabBack();
			break;
		}
		case "d": moveTo(originTop() + arg(n, 0, 1) - 1, cur.col); break;
		case "g":
			if ((n[0] || 0) === 3)
				tabs = new Array(cols).fill(false);
			else
				tabs[cur.col] = false;
			break;
		case "h": setMode(n, true); break;
		case "l": setMode(n, false); break;
		case "m": applySgr(n); break;
		case "n":
			/*
			 * Device Status Report. A program that asks where the
			 * cursor is and never hears back does not degrade, it
			 * HANGS -- it is blocking on a read. Answering is not a
			 * nicety.
			 */
			if ((n[0] || 0) === 6)
				onInput("\x1b[" + (cur.row - originTop() + 1) + ";" + (cur.col + 1) + "R");
			else if ((n[0] || 0) === 5)
				onInput("\x1b[0n");
			break;
		case "c":
			/* Device Attributes: "a VT102". Same blocking-read reason
			 * as DSR above. */
			onInput("\x1b[?6c");
			break;
		case "r":
			scrollTop = arg(n, 0, 1) - 1;
			scrollBot = (n[1] === undefined || n[1] === 0) ? rows - 1 : n[1] - 1;
			scrollTop = Math.max(0, Math.min(rows - 1, scrollTop));
			scrollBot = Math.max(scrollTop, Math.min(rows - 1, scrollBot));
			moveTo(originTop(), 0);
			break;
		case "s": saved = { row: cur.row, col: cur.col, sgr: Object.assign({}, sgr) }; break;
		case "u":
			if (saved !== null)
				moveTo(saved.row, saved.col), (sgr = saved.sgr);
			break;
		default:
			/* Unknown final byte: consumed and dropped, never printed.
			 * Leaking an unhandled sequence into the grid as literal
			 * text is worse than ignoring it -- it corrupts the screen
			 * AND hides which sequence was unsupported. */
			break;
		}
	}

	function tabForward() {
		let c = cur.col + 1;

		while (c < cols - 1 && !tabs[c])
			c++;
		cur.col = Math.min(c, cols - 1);
		wrapPending = false;
	}

	function tabBack() {
		let c = cur.col - 1;

		while (c > 0 && !tabs[c])
			c--;
		cur.col = Math.max(c, 0);
	}

	function feed(text) {
		for (let i = 0; i < text.length; i++) {
			const ch = text[i];
			const code = text.charCodeAt(i);

			switch (state) {
			case "ground":
				if (ch === "\x1b") { state = "esc"; }
				else if (ch === "\r") { cur.col = 0; wrapPending = false; }
				else if (ch === "\n" || ch === "\x0b" || ch === "\x0c") { lineFeed(); }
				else if (ch === "\b") {
					if (wrapPending) wrapPending = false;
					else if (cur.col > 0) cur.col--;
				}
				else if (ch === "\t") { tabForward(); }
				else if (ch === "\x0e") { charsetShifted = true; }
				else if (ch === "\x0f") { charsetShifted = false; }
				else if (code === 0x07) { /* bell: nothing to ring */ }
				else if (code >= 0x20 && code !== 0x7f) { putChar(ch); }
				break;

			case "esc":
				if (ch === "[") { state = "csi"; csiParams = ""; csiPrefix = ""; }
				else if (ch === "]") { state = "osc"; oscBuf = ""; }
				else if (ch === "P" || ch === "^" || ch === "_") { state = "dcs"; }
				else if (ch === "(" || ch === ")") { state = ch === "(" ? "charsetG0" : "charsetG1"; }
				else if (ch === "7") { saved = { row: cur.row, col: cur.col, sgr: Object.assign({}, sgr) }; state = "ground"; }
				else if (ch === "8") {
					if (saved !== null) { moveTo(saved.row, saved.col); sgr = saved.sgr; }
					state = "ground";
				}
				else if (ch === "D") { lineFeed(); state = "ground"; }
				else if (ch === "M") { reverseIndex(); state = "ground"; }
				else if (ch === "E") { lineFeed(); cur.col = 0; state = "ground"; }
				else if (ch === "H") { tabs[cur.col] = true; state = "ground"; }
				else if (ch === "c") { initScreen(cols, rows); sgr = { fg: null, bg: null, at: 0 }; state = "ground"; }
				else if (ch === "=" || ch === ">") { state = "ground"; }
				else if (ch === "#") { state = "escHash"; }
				else { state = "ground"; }
				break;

			case "escHash":
				state = "ground";
				break;

			case "charsetG0":
				charsetG0 = ch;
				state = "ground";
				break;

			case "charsetG1":
				state = "ground";
				break;

			case "csi":
				if (csiParams === "" && (ch === "?" || ch === ">" || ch === "<" || ch === "!")) {
					csiPrefix = ch;
				} else if ((code >= 0x30 && code <= 0x3b)) {
					csiParams += ch;
				} else if (code >= 0x20 && code <= 0x2f) {
					/* intermediate byte -- collected only so it cannot
					 * be mistaken for a final byte */
				} else if (code >= 0x40 && code <= 0x7e) {
					execCsi(ch);
					state = "ground";
				} else {
					state = "ground";
				}
				break;

			case "osc":
				/* Terminated by BEL or by ST (ESC \). The window title
				 * is the only thing that ever arrives here and there is
				 * nowhere in this dashboard to put it, so the payload
				 * is parsed to completion and dropped -- parsed rather
				 * than skipped, because an unterminated OSC would
				 * otherwise swallow the rest of the screen. */
				if (code === 0x07) { state = "ground"; }
				else if (ch === "\x1b") { state = "oscEsc"; }
				else { oscBuf += ch; }
				break;

			case "oscEsc":
				state = "ground";
				break;

			case "dcs":
				if (ch === "\x1b")
					state = "dcsEsc";
				break;

			case "dcsEsc":
				state = ch === "\\" ? "ground" : "dcs";
				break;
			}
		}
		render();
	}

	/* --- rendering --- */

	function styleOf(cell) {
		let fg = cell.fg;
		let bg = cell.bg;

		if (cell.at & VT_REVERSE) {
			const t = fg;

			fg = bg === null ? "default-bg" : bg;
			bg = t === null ? "default-fg" : t;
		}
		if (cell.at & VT_HIDDEN)
			fg = bg;
		return { fg: fg, bg: bg, at: cell.at };
	}

	function colorCss(v, isBg) {
		if (v === null || v === undefined)
			return null;
		if (v === "default-fg")
			return "var(--paper)";
		if (v === "default-bg")
			return "var(--carbon)";
		if (typeof v === "object")
			return "rgb(" + v.r + "," + v.g + "," + v.b + ")";
		return VT_PALETTE[v] || (isBg ? null : "var(--paper)");
	}

	function sameStyle(a, b) {
		if (a.at !== b.at)
			return false;
		return sameColor(a.fg, b.fg) && sameColor(a.bg, b.bg);
	}

	function sameColor(a, b) {
		if (a === b)
			return true;
		if (typeof a === "object" && typeof b === "object" && a !== null && b !== null)
			return a.r === b.r && a.g === b.g && a.b === b.b;
		return false;
	}

	function applyStyle(span, st) {
		const fg = colorCss(st.fg, false);
		const bg = colorCss(st.bg, true);

		if (fg !== null)
			span.style.color = fg;
		if (bg !== null)
			span.style.background = bg;
		if (st.at & VT_BOLD)
			span.style.fontWeight = "700";
		if (st.at & VT_DIM)
			span.style.opacity = "0.6";
		if (st.at & VT_ITALIC)
			span.style.fontStyle = "italic";
		if (st.at & (VT_UNDERLINE | VT_STRIKE)) {
			const parts = [];

			if (st.at & VT_UNDERLINE) parts.push("underline");
			if (st.at & VT_STRIKE) parts.push("line-through");
			span.style.textDecoration = parts.join(" ");
		}
	}

	/*
	 * One row into one div. Cells with identical styling are emitted as a
	 * single span, which is what keeps a full screen to a few dozen nodes
	 * per row instead of one per cell.
	 *
	 * Text is always set through textContent, never innerHTML: every byte
	 * here came from a process inside a container and is untrusted by
	 * definition.
	 */
	function renderRowInto(div, row, cursorCol) {
		div.textContent = "";

		let i = 0;

		while (i < row.length) {
			const st = styleOf(row[i]);
			let text = row[i].c;
			let j = i + 1;

			/* The cursor cell is always its own span so it can be
			 * inverted without splitting styles anywhere else. */
			if (i !== cursorCol) {
				while (j < row.length && j !== cursorCol && sameStyle(styleOf(row[j]), st)) {
					text += row[j].c;
					j++;
				}
			}

			const isCursor = i === cursorCol;

			if (st.fg === null && st.bg === null && st.at === 0 && !isCursor) {
				div.appendChild(document.createTextNode(text));
			} else {
				const span = document.createElement("span");

				applyStyle(span, st);
				if (isCursor)
					span.className = "vt-cursor";
				span.textContent = text;
				div.appendChild(span);
			}
			i = j;
		}
	}

	let cursorRendered = -1;

	function render() {
		const showCursor = mode.cursorVisible;
		const curRow = cur.row;

		/* The row the cursor left needs redrawing as much as the one it
		 * arrived at, or the old block stays painted behind it. */
		if (cursorRendered !== curRow) {
			markDirty(cursorRendered);
			markDirty(curRow);
			cursorRendered = curRow;
		} else {
			markDirty(curRow);
		}

		for (let r = 0; r < rows; r++) {
			if (!dirty[r])
				continue;
			renderRowInto(rowEls[r], buf[r],
			              (showCursor && r === curRow) ? Math.min(cur.col, cols - 1) : -1);
			dirty[r] = false;
		}
		if (!mode.altScreen)
			hostEl.scrollTop = hostEl.scrollHeight;
	}

	/* --- sizing --- */

	function measureCell() {
		const probe = document.createElement("div");

		probe.className = "vt-row";
		probe.style.position = "absolute";
		probe.style.visibility = "hidden";
		probe.style.whiteSpace = "pre";
		/* Measured over many characters and divided down: a single
		 * character's rounded width accumulates into a visibly wrong
		 * column count by the right-hand edge of a wide terminal. */
		probe.textContent = "M".repeat(100);
		hostEl.appendChild(probe);
		const rect = probe.getBoundingClientRect();
		const w = rect.width / 100;
		const h = rect.height;

		hostEl.removeChild(probe);
		return { w: w, h: h };
	}

	function fit() {
		const cell = measureCell();

		if (!(cell.w > 0) || !(cell.h > 0))
			return { cols: cols, rows: rows };

		const style = window.getComputedStyle(hostEl);
		const padX = parseFloat(style.paddingLeft) + parseFloat(style.paddingRight);
		const padY = parseFloat(style.paddingTop) + parseFloat(style.paddingBottom);
		const newCols = Math.max(20, Math.floor((hostEl.clientWidth - padX) / cell.w));
		const newRows = Math.max(5, Math.floor((hostEl.clientHeight - padY) / cell.h));

		if (newCols === cols && newRows === rows)
			return { cols: cols, rows: rows };

		resize(newCols, newRows);
		onResize(cols, rows);
		return { cols: cols, rows: rows };
	}

	function resize(newCols, newRows) {
		const oldRows = rows;

		/* Rows are kept by value, not reallocated: a resize that
		 * discarded the screen would blank a running program until its
		 * next full redraw, and a program that only repaints what
		 * changed might never issue one. */
		for (const row of buf) {
			while (row.length < newCols)
				row.push(blankCell());
			if (row.length > newCols)
				row.length = newCols;
		}
		while (buf.length < newRows)
			buf.push(blankRow(newCols));
		while (buf.length > newRows) {
			/* Shrinking drops from the top, and those lines are history
			 * in the ordinary sense, so they go to scrollback rather
			 * than being destroyed. */
			const leaving = buf.shift();

			if (!mode.altScreen)
				pushScrollback(leaving);
			if (cur.row > 0)
				cur.row--;
		}

		cols = newCols;
		rows = newRows;
		scrollTop = 0;
		scrollBot = rows - 1;
		cur.row = Math.min(cur.row, rows - 1);
		cur.col = Math.min(cur.col, cols - 1);
		if (oldRows !== rows || rowEls.length !== rows)
			rebuildRowEls();
		resetTabs();
		dirty = new Array(rows).fill(true);
		render();
	}

	/* --- input --- */

	function cursorSeq(letter) {
		/* Application cursor keys (DECCKM) send SS3 rather than CSI.
		 * Getting this wrong is why arrow keys insert letters instead
		 * of moving in vim and in a readline prompt. */
		return (mode.appCursor ? "\x1bO" : "\x1b[") + letter;
	}

	const KEYS = {
		ArrowUp: () => cursorSeq("A"),
		ArrowDown: () => cursorSeq("B"),
		ArrowRight: () => cursorSeq("C"),
		ArrowLeft: () => cursorSeq("D"),
		Home: () => cursorSeq("H"),
		End: () => cursorSeq("F"),
		Insert: () => "\x1b[2~",
		Delete: () => "\x1b[3~",
		PageUp: () => "\x1b[5~",
		PageDown: () => "\x1b[6~",
		Enter: () => "\r",
		Tab: () => "\t",
		Backspace: () => "\x7f",
		Escape: () => "\x1b",
		F1: () => "\x1bOP", F2: () => "\x1bOQ", F3: () => "\x1bOR", F4: () => "\x1bOS",
		F5: () => "\x1b[15~", F6: () => "\x1b[17~", F7: () => "\x1b[18~", F8: () => "\x1b[19~",
		F9: () => "\x1b[20~", F10: () => "\x1b[21~", F11: () => "\x1b[23~", F12: () => "\x1b[24~"
	};

	function onKeyDown(event) {
		let data = null;

		if (event.ctrlKey && !event.altKey && event.key.length === 1) {
			const up = event.key.toUpperCase();
			const code = up.charCodeAt(0);

			if (code >= 64 && code <= 95)
				data = String.fromCharCode(code - 64);
			else if (event.key === " ")
				data = "\x00";
			else if (event.key === "?")
				data = "\x7f";
		} else if (KEYS[event.key] !== undefined) {
			data = KEYS[event.key]();
		} else if (event.key.length === 1 && !event.metaKey) {
			/* Alt sends the key prefixed with ESC, which is how a
			 * terminal has always spelled Meta. */
			data = event.altKey ? "\x1b" + event.key : event.key;
		}

		if (data !== null) {
			event.preventDefault();
			onInput(data);
		}
	}

	function onPaste(event) {
		const text = (event.clipboardData || window.clipboardData).getData("text");

		if (!text)
			return;
		event.preventDefault();
		/* Bracketed paste lets the program tell typed input from pasted
		 * input, which is what stops an editor auto-indenting a pasted
		 * block into a staircase. */
		if (mode.bracketedPaste)
			onInput("\x1b[200~" + text + "\x1b[201~");
		else
			onInput(text);
	}

	function mouseButton(event, release) {
		if (release)
			return 3;
		if (event.button === 1)
			return 1;
		if (event.button === 2)
			return 2;
		return 0;
	}

	function cellFromEvent(event) {
		const cell = measureCell();
		const style = window.getComputedStyle(hostEl);
		const rect = screenEl.getBoundingClientRect();
		const c = Math.floor((event.clientX - rect.left - parseFloat(style.paddingLeft)) / cell.w);
		const r = Math.floor((event.clientY - rect.top) / cell.h);

		return {
			col: Math.max(0, Math.min(cols - 1, c)),
			row: Math.max(0, Math.min(rows - 1, r))
		};
	}

	function onMouse(event, release) {
		if (mode.mouse === 0)
			return;
		const pos = cellFromEvent(event);
		const b = mouseButton(event, release);

		event.preventDefault();
		if (mode.mouseSgr) {
			onInput("\x1b[<" + b + ";" + (pos.col + 1) + ";" + (pos.row + 1) +
			        (release ? "m" : "M"));
		} else {
			/* The original X10 encoding adds 32 to everything and so
			 * cannot express a coordinate past 223. Only used when the
			 * program never asked for SGR mode. */
			onInput("\x1b[M" + String.fromCharCode(32 + b, 32 + pos.col + 1, 32 + pos.row + 1));
		}
	}

	const keyHandler = (e) => onKeyDown(e);
	const pasteHandler = (e) => onPaste(e);
	const downHandler = (e) => onMouse(e, false);
	const upHandler = (e) => onMouse(e, true);

	hostEl.addEventListener("keydown", keyHandler);
	hostEl.addEventListener("paste", pasteHandler);
	hostEl.addEventListener("mousedown", downHandler);
	hostEl.addEventListener("mouseup", upHandler);

	let observer = null;

	/* autoFit is off only for tests, which need a size they chose rather
	 * than one measured from a headless layout. Every real caller wants
	 * the observer: a pane that changes size and does not tell the
	 * remote program is the bug ADR-0242 exists to prevent, one layer
	 * further out. */
	if ((!opts || opts.autoFit !== false) && typeof ResizeObserver !== "undefined") {
		observer = new ResizeObserver(() => fit());
		observer.observe(hostEl);
	}

	initScreen(cols, rows);

	return {
		feed: feed,
		fit: fit,
		resize: resize,
		size: () => ({ cols: cols, rows: rows }),
		cursor: () => ({ row: cur.row, col: cur.col, visible: mode.cursorVisible }),
		isAltScreen: () => mode.altScreen,
		rowText: (r) => buf[r].map((c) => c.c).join(""),
		dispose: function () {
			hostEl.removeEventListener("keydown", keyHandler);
			hostEl.removeEventListener("paste", pasteHandler);
			hostEl.removeEventListener("mousedown", downHandler);
			hostEl.removeEventListener("mouseup", upHandler);
			if (observer !== null)
				observer.disconnect();
		}
	};
}
