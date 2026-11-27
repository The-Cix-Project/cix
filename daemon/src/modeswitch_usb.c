#include "modeswitch_usb.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/*
 * ADR-0398: the transfers of one switch, over usbfs, in a helper child.
 *
 * Every transfer carries a timeout, and nothing here waits for the
 * device to come back: the helper sends and returns (#689), and the
 * target id's own add uevent tells the daemon the switch worked.
 *
 * The usbfs structs are the kernel's own (linux/usbdevice_fs.h), not
 * redeclared; none of them is packed, and their layout is asserted
 * here so a header that changed under us fails the build rather than
 * the transfer. Descriptors are read as bytes at their USB 2.0 chapter
 * 9 offsets, never through a struct.
 */

_Static_assert(offsetof(struct usbdevfs_bulktransfer, data) == 16 &&
                       sizeof(struct usbdevfs_bulktransfer) == 24,
               "usbdevfs_bulktransfer layout");
_Static_assert(offsetof(struct usbdevfs_ctrltransfer, timeout) == 8 &&
                       offsetof(struct usbdevfs_ctrltransfer, data) == 16 &&
                       sizeof(struct usbdevfs_ctrltransfer) == 24,
               "usbdevfs_ctrltransfer layout");

#define TRANSFER_TIMEOUT_MS 3000
#define CONTROL_TIMEOUT_MS 1000
#define DESC_MAX 4096
#define WAIT_BEFORE_MAX_S 10
#define RELEASE_DELAY_MAX_MS 10000

static void sleep_ms(int ms)
{
	struct timespec ts;

	if (ms <= 0)
		return;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

static int control_in(int fd, unsigned char type, unsigned char req, unsigned short value,
                      unsigned short index, unsigned char *buf, unsigned short len)
{
	struct usbdevfs_ctrltransfer c;

	memset(&c, 0, sizeof(c));
	c.bRequestType = type;
	c.bRequest = req;
	c.wValue = value;
	c.wIndex = index;
	c.wLength = len;
	c.timeout = CONTROL_TIMEOUT_MS;
	c.data = buf;
	return ioctl(fd, USBDEVFS_CONTROL, &c);
}

static int bulk(int fd, unsigned int ep, void *buf, unsigned int len)
{
	struct usbdevfs_bulktransfer b;

	memset(&b, 0, sizeof(b));
	b.ep = ep;
	b.len = len;
	b.timeout = TRANSFER_TIMEOUT_MS;
	b.data = buf;
	return ioctl(fd, USBDEVFS_BULK, &b);
}

/* The device answered a transfer by leaving: it has switched. */
static int device_left(int err)
{
	return err == ENODEV || err == ESHUTDOWN;
}

/*
 * The interface's bulk endpoints, from the active configuration's
 * descriptors: usbfs read() returns the device descriptor and then each
 * configuration descriptor with everything below it. 0 when the
 * interface was found.
 */
static int find_endpoints(const unsigned char *d, size_t n, int config_value, int ifno,
                          int *ep_out, int *ep_in)
{
	size_t at = 18; /* the device descriptor */
	int in_config = 0, in_iface = 0, found = 0;

	*ep_out = -1;
	*ep_in = -1;
	while (at + 2 <= n) {
		unsigned char len = d[at], type = d[at + 1];

		if (len < 2 || at + len > n)
			break;
		if (type == 2 && len >= 9) { /* configuration: bConfigurationValue at 5 */
			in_config = config_value < 0 || d[at + 5] == config_value;
			in_iface = 0;
		} else if (type == 4 && len >= 9 && in_config) { /* interface: number 2, alt 3 */
			in_iface = d[at + 2] == ifno && d[at + 3] == 0;
			if (in_iface)
				found = 1;
		} else if (type == 5 && len >= 7 && in_config && in_iface) {
			int addr = d[at + 2], attrs = d[at + 3];

			if ((attrs & 3) == 2) { /* bulk */
				if ((addr & 0x80) != 0 && *ep_in < 0)
					*ep_in = addr;
				else if ((addr & 0x80) == 0 && *ep_out < 0)
					*ep_out = addr;
			}
		}
		at += len;
	}
	return found ? 0 : -1;
}

static void say(struct modeswitch_result *res, const char *what, int err)
{
	snprintf(res->error, sizeof(res->error), "%s: %s", what, strerror(err));
}

int modeswitch_usb_run(const char *dev_path, const struct modeswitch_rule *r,
                       struct modeswitch_result *res)
{
	static unsigned char desc[DESC_MAX];
	static struct modeswitch_step steps[MODESWITCH_STEPS_MAX];
	unsigned char csw[MODESWITCH_CSW_LEN], scratch[4096], cfg = 0, maxlun = 0;
	int fd, ifno = r->interface, ep_out, ep_in, nsteps, i, config_value = -1;
	ssize_t n;

	memset(res, 0, sizeof(*res));
	sleep_ms((r->wait_before_s > WAIT_BEFORE_MAX_S ? WAIT_BEFORE_MAX_S : r->wait_before_s) *
	         1000);

	fd = open(dev_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		say(res, "open", errno);
		return -1;
	}
	n = read(fd, desc, sizeof(desc));
	if (n < 18) {
		say(res, "reading the descriptors", n < 0 ? errno : EIO);
		close(fd);
		return -1;
	}
	/* GET_CONFIGURATION: which configuration's interfaces are live. */
	if (control_in(fd, 0x80, 0x08, 0, 0, &cfg, 1) == 1 && cfg != 0)
		config_value = cfg;
	if (find_endpoints(desc, (size_t)n, config_value, ifno, &ep_out, &ep_in) != 0) {
		snprintf(res->error, sizeof(res->error), "interface %d is not in the active configuration",
		         ifno);
		close(fd);
		return -1;
	}
	if (r->message_endpoint >= 0)
		ep_out = r->message_endpoint;
	if (r->response_endpoint >= 0)
		ep_in = r->response_endpoint;
	if (ep_out < 0) {
		snprintf(res->error, sizeof(res->error), "interface %d has no bulk OUT endpoint", ifno);
		close(fd);
		return -1;
	}

	if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifno) != 0) {
		struct usbdevfs_ioctl cmd;

		if (errno != EBUSY) {
			say(res, "claiming the interface", errno);
			close(fd);
			return -1;
		}
		/* A driver holds it. The verdict refused any device with a disk
		 * below it, so this is never a disk's driver. */
		memset(&cmd, 0, sizeof(cmd));
		cmd.ifno = ifno;
		cmd.ioctl_code = USBDEVFS_DISCONNECT;
		if (ioctl(fd, USBDEVFS_IOCTL, &cmd) != 0 ||
		    ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifno) != 0) {
			say(res, "taking the interface from its driver", errno);
			close(fd);
			return -1;
		}
	}

	/* Bulk-only GET MAX LUN (class request 0xfe). A device with one LUN
	 * may stall it, which means LUN 0. */
	if (r->standard_eject && control_in(fd, 0xa1, 0xfe, 0, (unsigned short)ifno, &maxlun, 1) != 1)
		maxlun = 0;

	nsteps = modeswitch_plan(r, maxlun, steps);
	for (i = 0; i < nsteps; i++) {
		const struct modeswitch_step *s = &steps[i];

		if (bulk(fd, (unsigned int)ep_out, (void *)s->bytes, (unsigned int)s->len) < 0) {
			if (device_left(errno) && i > 0) {
				res->left_early = 1;
				break;
			}
			say(res, "sending a switch message", errno);
			res->steps_sent = i;
			ioctl(fd, USBDEVFS_RELEASEINTERFACE, &ifno);
			close(fd);
			return -1;
		}
		res->steps_sent = i + 1;
		if (ep_in < 0)
			continue;
		/* What follows a message is read and discarded: a device that
		 * switches may never send it, so no read decides anything, but
		 * one left unread would make the next command a phase error. */
		if (s->is_cbw && s->data_in && s->data_len > 0 &&
		    bulk(fd, (unsigned int)ep_in, scratch,
		         s->data_len < sizeof(scratch) ? s->data_len : (unsigned int)sizeof(scratch)) < 0 &&
		    device_left(errno)) {
			res->left_early = 1;
			break;
		}
		if (s->is_cbw && bulk(fd, (unsigned int)ep_in, csw, sizeof(csw)) < 0 &&
		    device_left(errno)) {
			res->left_early = 1;
			break;
		}
		if (s->read_response && bulk(fd, (unsigned int)ep_in, scratch, 512) < 0 &&
		    device_left(errno)) {
			res->left_early = 1;
			break;
		}
	}
	if (!res->left_early) {
		ioctl(fd, USBDEVFS_RELEASEINTERFACE, &ifno);
		sleep_ms(r->release_delay_ms > RELEASE_DELAY_MAX_MS ? RELEASE_DELAY_MAX_MS
		                                                    : r->release_delay_ms);
	}
	close(fd);
	res->ok = 1;
	return 0;
}
