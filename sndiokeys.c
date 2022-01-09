/*	$OpenBSD$	*/
/*
 * Copyright (c) 2014-2020 Alexandre Ratchov <alex@caoua.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sndio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

/*
 * X keysyms to increment / decrement volume: + and -
 */
#define KEY_INC	XK_plus
#define KEY_DEC	XK_minus
#define KEY_DEV	XK_0

/*
 * modifiers: Ctrl + Alt
 */
#define MODMASK		(ControlMask | Mod1Mask)

/*
 * volume increment
 */
#define VOL_INC	9

struct sioctl_hdl *hdl;
int verbose;

struct ctl {
	struct ctl *next;
	struct sioctl_desc desc;
	int val;
} *ctl_list;

/*
 * X bits
 */
Display	*dpy;
KeyCode inc_code, dec_code, dev_code;
KeySym *inc_map, *dec_map, *dev_map;

/*
 * new control
 */
static void
dev_ondesc(void *unused, struct sioctl_desc *desc, int val)
{
	struct ctl *i, **pi;

	if (desc == NULL)
		return;

	for (pi = &ctl_list; (i = *pi) != NULL; pi = &i->next) {
		if (desc->addr == i->desc.addr) {
			*pi = i->next;
			free(i);
			break;
		}
	}

	switch (desc->type) {
	case SIOCTL_NUM:
	case SIOCTL_SW:
	case SIOCTL_VEC:
	case SIOCTL_LIST:
	case SIOCTL_SEL:
		break;
	default:
		return;
	}

	i = malloc(sizeof(struct ctl));
	if (i == NULL) {
		perror("malloc");
		exit(1);
	}
	i->desc = *desc;
	i->val = val;
	i->next = *pi;
	*pi = i;
}

/*
 * control value changed
 */
static void
dev_onval(void *unused, unsigned int addr, unsigned int val)
{
	struct ctl *i, *j;

	i = ctl_list;
	for (;;) {
		if (i == NULL)
			return;
		if (i->desc.addr == addr)
			break;
		i = i->next;
	}

	if (i->desc.type == SIOCTL_SEL) {
		for (j = ctl_list; j != NULL; j = j->next) {
			if (strcmp(i->desc.group, j->desc.group) != 0 ||
			    strcmp(i->desc.node0.name, j->desc.node0.name) != 0 ||
			    strcmp(i->desc.func, j->desc.func) != 0 ||
			    i->desc.node0.unit != j->desc.node0.unit)
				continue;
			j->val = (i->desc.addr == j->desc.addr);
		}
	} else
		i->val = val;
}

/*
 * send output volume message and to the server
 */
static void
dev_incrvol(int incr)
{
	int vol;
	struct ctl *i;

	for (i = ctl_list; i != NULL; i = i->next) {
		if (i->desc.group[0] != 0 ||
		    strcmp(i->desc.node0.name, "output") != 0 ||
		    strcmp(i->desc.func, "level") != 0)
			continue;

		vol = i->val + incr;
		if (vol < 0)
			vol = 0;
		if (vol > i->desc.maxval)
			vol = i->desc.maxval;
		if (i->val != vol) {
			if (verbose)
				fprintf(stderr, "setting level to %d\n", vol);
			i->val = vol;
			sioctl_setval(hdl, i->desc.addr, vol);
		}
        }
}

/*
 * send output volume message and to the server
 */
static void
dev_seldev()
{
	struct ctl *i, *j;

	i = ctl_list;
	while (1) {
		if (i == NULL)
			return;
		if (i->desc.group[0] == 0 &&
		    strcmp(i->desc.node0.name, "server") == 0 &&
		    strcmp(i->desc.func, "device") == 0 &&
		    i->val == 1)
			break;
		i = i->next;
	}

	j = i;
	while (1) {
		j = j->next;
		if (j == NULL)
			j = ctl_list;
		if (j->desc.addr == i->desc.addr)
			return;
		if (j->desc.group[0] == 0 &&
		    strcmp(j->desc.node0.name, "server") == 0 &&
		    strcmp(j->desc.func, "device") == 0 &&
		    j->val == 0)
			break;
	}

	if (verbose)
		fprintf(stderr, "setting device to %s\n", j->desc.node1.name);

	i->val = 0;
	j->val = 1;

	sioctl_setval(hdl, j->desc.addr, 1);
}

/*
 * register hot-keys in X
 */
static void
grab_keys(void)
{
	unsigned int i, scr, nscr;
	int nret;

	if (verbose)
		fprintf(stderr, "grabbing keys\n");

	inc_code = XKeysymToKeycode(dpy, KEY_INC);
	inc_map = XGetKeyboardMapping(dpy, inc_code, 1, &nret);
	if (nret <= ShiftMask) {
		fprintf(stderr, "couldn't get keymap for '+' key\n");
		exit(1);
	}

	dec_code = XKeysymToKeycode(dpy, KEY_DEC);
	dec_map = XGetKeyboardMapping(dpy, dec_code, 1, &nret);
	if (nret <= ShiftMask) {
		fprintf(stderr, "couldn't get keymap for '-' key\n");
		exit(1);
	}

	dev_code = XKeysymToKeycode(dpy, KEY_DEV);
	dev_map = XGetKeyboardMapping(dpy, dev_code, 1, &nret);
	if (nret <= ShiftMask) {
		fprintf(stderr, "couldn't get keymap for '0' key\n");
		exit(1);
	}

	nscr = ScreenCount(dpy);
	for (i = 0; i <= 0xff; i++) {
		if ((i & MODMASK) != 0)
			continue;
		for (scr = 0; scr != nscr; scr++) {

			XGrabKey(dpy, inc_code, i | MODMASK,
			    RootWindow(dpy, scr), 1,
			    GrabModeAsync, GrabModeAsync);

			XGrabKey(dpy, dec_code, i | MODMASK,
			    RootWindow(dpy, scr), 1,
			    GrabModeAsync, GrabModeAsync);

			XGrabKey(dpy, dev_code, i | MODMASK,
			    RootWindow(dpy, scr), 1,
			    GrabModeAsync, GrabModeAsync);
		}
	}
}

/*
 * unregister hot-keys
 */
static void
ungrab_keys(void)
{
	unsigned int scr, nscr;

	if (verbose)
		fprintf(stderr, "ungrabbing keys\n");

	XFree(inc_map);
	XFree(dec_map);

	nscr = ScreenCount(dpy);
	for (scr = 0; scr != nscr; scr++)
		XUngrabKey(dpy, AnyKey, AnyModifier, RootWindow(dpy, scr));
}

int
main(int argc, char **argv)
{
	int scr;
	XEvent xev;
	int c, nfds;
	int background;
	struct ctl *i;
	char *dev_name;
	struct pollfd *pfds;

	/*
	 * parse command line options
	 */
	dev_name = SIO_DEVANY;
	verbose = 0;
	background = 0;
	while ((c = getopt(argc, argv, "Df:q:v")) != -1) {
		switch (c) {
		case 'D':
			background = 1;
			break;
		case 'v':
			verbose++;
			break;
		case 'q': /* compat */
		case 'f':
			dev_name = optarg;
			break;
		default:
			goto bad_usage;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 0) {
	bad_usage:
		fprintf(stderr, "usage: sndiokeys [-Dv] [-f device]\n");
		exit(1);
	}

	dpy = XOpenDisplay(NULL);
	if (dpy == 0) {
		fprintf(stderr, "Couldn't open display\n");
		exit(1);
	}

	hdl = sioctl_open(dev_name, SIOCTL_READ | SIOCTL_WRITE, 0);
	if (hdl == NULL) {
		fprintf(stderr, "%s: couldn't open audio device\n", dev_name);
		return 0;
	}
	sioctl_ondesc(hdl, dev_ondesc, NULL);
	sioctl_onval(hdl, dev_onval, NULL);

	pfds = calloc(sioctl_nfds(hdl) + 1, sizeof(struct pollfd));
	if (pfds == NULL) {
		perror("calloc\n");
		exit(1);
	}

	/* mask non-key events for each screan */
	for (scr = 0; scr != ScreenCount(dpy); scr++)
		XSelectInput(dpy, RootWindow(dpy, scr), KeyPress);

	grab_keys();

	if (background) {
		verbose = 0;
		if (daemon(0, 0) < 0) {
			perror("daemon");
			exit(1);
		}
	}

	while (1) {
		while (XPending(dpy)) {
			XNextEvent(dpy, &xev);
			if (xev.type == MappingNotify) {
				if (xev.xmapping.request != MappingKeyboard)
					continue;
				if (verbose)
					fprintf(stderr, "keyboard remapped\n");
				ungrab_keys();
				grab_keys();
			}
			if (xev.type != KeyPress)
				continue;
			if (xev.xkey.keycode == inc_code &&
			    inc_map[xev.xkey.state & ShiftMask] == KEY_INC) {
				dev_incrvol(VOL_INC);
			} else if (xev.xkey.keycode == dec_code &&
			    dec_map[xev.xkey.state & ShiftMask] == KEY_DEC) {
				dev_incrvol(-VOL_INC);
			} else if (xev.xkey.keycode == dev_code &&
			    dev_map[xev.xkey.state & ShiftMask] == KEY_DEV) {
				dev_seldev();
			}
		}
		nfds = (hdl != NULL) ? sioctl_pollfd(hdl, pfds, 0) : 0;
		pfds[nfds].fd = ConnectionNumber(dpy);
		pfds[nfds].events = POLLIN;
		while (poll(pfds, nfds + 1, -1) < 0 && errno == EINTR)
			; /* nothing */
		if ((sioctl_revents(hdl, pfds) & POLLHUP) ||
		    (pfds[nfds].revents & POLLHUP))
			break;
	}

	XFree(inc_map);
	XFree(dec_map);
	XFree(dev_map);
	XCloseDisplay(dpy);

	sioctl_close(hdl);

	while ((i = ctl_list) != NULL) {
		ctl_list = ctl_list->next;
		free(i);
	}

	return 0;
}
