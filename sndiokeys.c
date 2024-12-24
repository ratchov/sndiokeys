/*	$OpenBSD$	*/
/*
 * Copyright (c) 2014-2021 Alexandre Ratchov <alex@caoua.org>
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sndio.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <X11/XKBlib.h>

/*
 * The mask of modifiers supported for key-bindings.
 */
#define MODMASK		(ControlMask | Mod1Mask | Mod4Mask)

/*
 * Number of level steps between 0 and 1
 */
#define NSTEP		20

/*
 * Max fds we poll
 */
#define MAXFDS		64


struct modname {
	unsigned int modmask;
	char *name;
} modname_tab[] = {
	{ControlMask, "Control"},
	{Mod1Mask, "Mod1"},
	{Mod4Mask, "Mod4"},
	{0, NULL}
};

struct ctl {
	struct ctl *next;
	struct sioctl_desc desc;
	int val;
} *ctl_list;

struct key {
	struct key *next;
	unsigned int modmask;
	KeySym sym;
	KeyCode code;
	KeySym *map;
	char *name;
	char *func;
	int dir;
} *key_list;

Display	*dpy;
int (*error_handler_xlib)(Display *, XErrorEvent *);
KeySym error_keysym;

char *dev_name;
struct sioctl_hdl *ctl_hdl;
int ctl_maxfds;
int maxfds;

int verbose;
int silent;
int beep_pending;
int audible_bell;

/*
 * Play a short beep. It's used as sonic feedback and/or keyboard bell
 */
static void
play_beep(void)
{
#define BELL_RATE	48000
#define BELL_LEN	(BELL_RATE / 20)
#define BELL_PERIOD	(BELL_RATE / 880)
#define BELL_AMP	(INT16_MAX / 32)
	int16_t data[BELL_LEN];
	struct sio_hdl *beep_hdl;
	struct sio_par par;
	int i;

	beep_hdl = sio_open(dev_name, SIO_PLAY, 0);
	if (beep_hdl == NULL) {
		if (verbose)
			fprintf(stderr, "bell: failed to open audio device\n");
		return;
	}

	sio_initpar(&par);
	par.bits = 16;
	par.rate = BELL_RATE;
	par.pchan = 1;

	if (!sio_setpar(beep_hdl, &par) || !sio_getpar(beep_hdl, &par)) {
		if (verbose)
			fprintf(stderr, "bell: failed to set parameters\n");
		goto err_close;
	}

	if (par.bits != 16 || par.bps != 2 || par.le != SIO_LE_NATIVE ||
	    par.pchan != 1 || par.rate != BELL_RATE) {
		if (verbose)
			fprintf(stderr, "bell: bad parameters\n");
		goto err_close;
	}

	if (!sio_start(beep_hdl)) {
		if (verbose)
			fprintf(stderr, "bell: failed to start playback\n");
		goto err_close;
	}

	for (i = 0; i < BELL_LEN; i++) {
		data[i] = (i % BELL_PERIOD) < (BELL_PERIOD / 2) ?
		    BELL_AMP : -BELL_AMP;
	}

	if (sio_write(beep_hdl, data, sizeof(data)) != sizeof(data)) {
		if (verbose)
			fprintf(stderr, "bell: short write\n");
		goto err_close;
	}

err_close:
	sio_close(beep_hdl);
}

/*
 * compare two sioctl_desc structures, used to sort ctl_list
 */
static int
cmpdesc(struct sioctl_desc *d1, struct sioctl_desc *d2)
{
	int res;

	res = strcmp(d1->group, d2->group);
	if (res != 0)
		return res;
	res = strcmp(d1->node0.name, d2->node0.name);
	if (res != 0)
		return res;
	res = d1->type - d2->type;
	if (res != 0)
		return res;
	res = strcmp(d1->func, d2->func);
	if (res != 0)
		return res;
	res = d1->node0.unit - d2->node0.unit;
	if (d1->type == SIOCTL_SEL) {
		if (res != 0)
			return res;
		res = strcmp(d1->node1.name, d2->node1.name);
		if (res != 0)
			return res;
		res = d1->node1.unit - d2->node1.unit;
	}
	return res;
}

/*
 * skip all parameters with the same group, name, and func
 */
static struct ctl *
nextctl(struct ctl *i)
{
	char *str, *group, *func;
	int unit;

	group = i->desc.group;
	func = i->desc.func;
	str = i->desc.node0.name;
	unit = i->desc.node0.unit;
	for (i = i->next; i != NULL; i = i->next) {
		if (strcmp(i->desc.group, group) != 0 ||
		    strcmp(i->desc.node0.name, str) != 0 ||
		    strcmp(i->desc.func, func) != 0 ||
		    i->desc.node0.unit != unit)
			return i;
	}
	return NULL;
}

/*
 * move to the next selector entry or retrun NULL
 */
static struct ctl *
nextent(struct ctl *i)
{
	char *str, *group, *func;
	int unit;

	group = i->desc.group;
	func = i->desc.func;
	str = i->desc.node0.name;
	unit = i->desc.node0.unit;
	for (i = i->next; i != NULL; i = i->next) {
		if (strcmp(i->desc.group, group) != 0 ||
		    strcmp(i->desc.node0.name, str) != 0 ||
		    strcmp(i->desc.func, func) != 0)
			return NULL;
		if (i->desc.node0.unit == unit)
			return i;
	}
	return NULL;
}

/*
 * sndio call-back for added/removed controls
 */
static void
ondesc(void *unused, struct sioctl_desc *desc, int val)
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
	case SIOCTL_SEL:
		break;
	default:
		return;
	}

	/*
	 * find the right position to insert the new widget
	 */
	for (pi = &ctl_list; (i = *pi) != NULL; pi = &i->next) {
		if (cmpdesc(desc, &i->desc) <= 0)
			break;
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
 * sndio call-back for control value/changes
 */
static void
onval(void *unused, unsigned int addr, unsigned int val)
{
	struct ctl *i, *j;

	if (verbose)
		fprintf(stderr, "onval: %d -> %d\n", addr, val);

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

static int
ctl_open(void)
{
	ctl_hdl = sioctl_open(dev_name, SIOCTL_READ | SIOCTL_WRITE, 0);
	if (ctl_hdl == NULL) {
		fprintf(stderr, "%s: couldn't open audio device\n", dev_name);
		return 0;
	}
	sioctl_ondesc(ctl_hdl, ondesc, NULL);
	sioctl_onval(ctl_hdl, onval, NULL);

	ctl_maxfds = sioctl_nfds(ctl_hdl);
	if (ctl_maxfds + maxfds >= MAXFDS) {
		fprintf(stderr, "%s: too many fds\n", dev_name);
		sioctl_close(ctl_hdl);
		ctl_hdl = NULL;
		return 0;
	}
	maxfds += ctl_maxfds;
	fprintf(stderr, "maxfds -> %d\n", maxfds);
	return 1;
}

static void
ctl_close(void)
{
	struct ctl *c;

	while ((c = ctl_list) != NULL) {
		ctl_list = ctl_list->next;
		free(c);
	}
	maxfds -= ctl_maxfds;
	fprintf(stderr, "maxfds -> %d\n", maxfds);
	sioctl_close(ctl_hdl);
	ctl_hdl = NULL;
}

static void
setval_sel(struct ctl *first, int dir)
{
	struct ctl *cur, *next;

	if (dir != 0)
		return;

	/* find the current entry */

	cur = first;
	while (cur->val == 0) {
		cur = nextent(cur);
		if (cur == NULL) {
			fprintf(stderr, "no current value\n");
			return;
		}
	}

	/* find the next entry */

	next = nextent(cur);
	if (next == NULL)
		next = first;
	if (next == cur) {
		fprintf(stderr, "no next value\n");
		return;
	}

	if (verbose)
		fprintf(stderr, "%d -> %s\n", next->desc.addr, next->desc.node1.name);

	cur->val = 0;
	next->val = 1;
	sioctl_setval(ctl_hdl, next->desc.addr, 1);
	if (!silent)
		beep_pending = 1;
}

static void
setval_num(struct ctl *i, int dir)
{
	int val, incr;

	if (i->desc.maxval > 1 && dir != 0) {
		incr = ((int)i->desc.maxval + NSTEP - 1) / NSTEP;
		val = i->val + dir * incr;
		if (val < 0)
			val = 0;
		if (val > i->desc.maxval)
			val = i->desc.maxval;
		if (val == i->val)
			return;
	} else if (i->desc.maxval == 1 && dir == 0) {
		val = i->val ^ 1;
	} else
		return;

	if (verbose)
		fprintf(stderr, "num: %d -> %d\n", i->desc.addr, val);

	i->val = val;
	sioctl_setval(ctl_hdl, i->desc.addr, val);
	if (!silent)
		beep_pending = 1;
}

/*
 * change the control
 */
static void
setval(char *name, char *func, int dir)
{
	struct ctl *i;

	if (!ctl_hdl) {
		if (!ctl_open())
			return;
	}

	i = ctl_list;
	while (1) {
		if (i == NULL)
			return;
		if (i->desc.group[0] == 0 &&
		    strcmp(i->desc.node0.name, name) == 0 &&
		    strcmp(i->desc.func, func) == 0) {
			if (i->desc.type == SIOCTL_SEL)
				setval_sel(i, dir);
			else
				setval_num(i, dir);
		}
		i = nextctl(i);
	}
}

/*
 * error handler for Xlib functions. Print a meaningful
 * message for well-known errors and exit.
 */
static int
error_handler(Display *d, XErrorEvent *e)
{
	if (e->request_code == X_GrabKey && e->error_code == BadAccess) {
		fprintf(stderr, "Key \"%s\" already grabbed by another program\n",
		    XKeysymToString(error_keysym));
		exit(1);
	}

	return error_handler_xlib(d, e);
}

/*
 * register hot-keys
 */
static void
grab_keys(void)
{
	struct key *key;
	unsigned int i, scr, nscr;
	int nret;

	for (key = key_list; key != NULL; key = key->next) {

		key->code = XKeysymToKeycode(dpy, key->sym);
		key->map = XGetKeyboardMapping(dpy, key->code, 1, &nret);
		if (nret <= ShiftMask) {
			fprintf(stderr, "%s: couldn't get keymap for key\n",
			    XKeysymToString(key->sym));
			exit(1);
		}

		/*
		 * Grab the key for all the modifier combinations
		 * whose MODMASK bits match exactly. This way
		 * X sends the events regardless of the state
		 * of the other modifiers: Shift, Caps Lock,
		 * Num Lock, Scroll Lock and Mode switch.
		 */
		error_keysym = key->sym;
		nscr = ScreenCount(dpy);
		for (i = 0; i <= 0xff; i++) {
			if ((i & MODMASK) != key->modmask)
				continue;
			for (scr = 0; scr != nscr; scr++) {
				XGrabKey(dpy, key->code, i,
				    RootWindow(dpy, scr), 1,
				    GrabModeAsync, GrabModeAsync);
			}
		}
		XSync(dpy, False);
	}
}

/*
 * unregister hot-keys
 */
static void
ungrab_keys(void)
{
	struct key *key;
	unsigned int scr, nscr;

	for (key = key_list; key != NULL; key = key->next) {
		XFree(key->map);
	}

	nscr = ScreenCount(dpy);
	for (scr = 0; scr != nscr; scr++)
		XUngrabKey(dpy, AnyKey, AnyModifier, RootWindow(dpy, scr));
}

/*
 * add key binding, removing old binding for the same function
 */
static void
add_key(unsigned int modmask, KeySym sym, char *name, char *func, int dir)
{
	struct key *key, **p;

	/* delete existing bindings for the same function */
	p = &key_list;
	while ((key = *p) != NULL) {
		if (strcmp(key->name, name) == 0 &&
		    strcmp(key->func, func) == 0 && key->dir == dir) {
			*p = key->next;
			free(key);
		} else
			p = &key->next;
	}

	key = malloc(sizeof(struct key));
	if (key == NULL) {
		perror("malloc: key");
		exit(1);
	}
	key->sym = sym;
	key->modmask = modmask;
	key->name = name;
	key->func = func;
	key->dir = dir;

	key->next = NULL;
	*p = key;
}

/*
 * parse key binding with this format:
 *
 *	[mod '+' mod '+' ...] key ':' name '.' func {'+' | '-' | '!'}
 */
static void
parsekey(char *str)
{
	char *p, *end, *name, *func;
	struct modname *mod;
	unsigned int modmask;
	KeySym keysym;
	int dir;

	name = strchr(str, ':');
	if (name == NULL) {
		fprintf(stderr, "%s: expected ':'\n", str);
		exit(1);
	}
	*name++ = 0;

	p = str;

	modmask = 0;
	while (1) {
		end = strchr(p, '+');
		if (end == NULL)
			break;
		*end = 0;

		mod = modname_tab;
		while (1) {
			if (mod->modmask == 0) {
				fprintf(stderr, "%s: bad modifier\n", p);
				exit(1);
			}
			if (strcmp(p, mod->name) == 0) {
				modmask |= mod->modmask;
				break;
			}
			mod++;
		}

		p = end + 1;
	}

	keysym = XStringToKeysym(p);
	if (keysym == NoSymbol) {
		fprintf(stderr, "%s: unknowm key\n", p);
		exit(1);
	}

	/*
	 * compat with old versions
	 */
	if (strcmp(name, "inc_level") == 0) {
		add_key(modmask, keysym, "output", "level", 1);
		return;
	} else if (strcmp(name, "dec_level") == 0) {
		add_key(modmask, keysym, "output", "level", -1);
		return;
	} else if (strcmp(name, "cycle_dev") == 0) {
		add_key(modmask, keysym, "server", "device", 0);
		return;
	}

	func = strchr(name, '.');
	if (func == NULL) {
		fprintf(stderr, "%s: expected '.'\n", name);
		exit(1);
	}
	*func++ = 0;

	if ((end = strchr(func, '+')) != NULL) {
		dir = 1;
	} else if ((end = strchr(func, '-')) != NULL) {
		dir = -1;
	} else if ((end = strchr(func, '!')) != NULL) {
		dir = 0;
	} else {
		fprintf(stderr, "%s: expected '+', '-' or '!'\n", end);
		exit(1);
	}
	*end++ = 0;

	if (*end != 0) {
		fprintf(stderr, "%s: junk at end of the argument\n", end);
		exit(1);
	}

	add_key(modmask, keysym, name, func, dir);
}

int
main(int argc, char **argv)
{
	int scr;
	XEvent xev;
	int c, nfds;
	int background;
	struct pollfd pfds[MAXFDS];
	struct key *key;
	int xkb, xkb_ev_base, xkb_auto_controls, xkb_auto_values;
	size_t ctl_nfds;

	dev_name = SIO_DEVANY;
	verbose = 0;
	background = 0;

	while ((c = getopt(argc, argv, "ab:Df:m:sv")) != -1) {
		switch (c) {
		case 'a':
			audible_bell = 1;
			break;
		case 'b':
			parsekey(optarg);
			break;
		case 'D':
			background = 1;
			break;
		case 'f':
			dev_name = optarg;
			break;
		case 's':
			silent = 1;
			break;
		case 'v':
			verbose++;
			break;
		default:
			goto bad_usage;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 0) {
	bad_usage:
		fputs("usage: sndiokeys "
		    "[-aDsv] "
		    "[-b [mod+...]key:control[+|-|!] "
		    "[-f device]\n",
		    stderr);
		exit(1);
	}

	if (key_list == NULL) {
		add_key(ControlMask | Mod1Mask, XK_plus, "output", "level", 1);
		add_key(ControlMask | Mod1Mask, XK_minus, "output", "level", -1);
		add_key(ControlMask | Mod1Mask, XK_0, "output", "mute", 0);
		add_key(ControlMask | Mod1Mask, XK_Tab, "server", "device", 0);
	}

	error_handler_xlib = XSetErrorHandler(error_handler);

	dpy = XOpenDisplay(NULL);
	if (dpy == 0) {
		fprintf(stderr, "Couldn't open display\n");
		exit(1);
	}
	maxfds++;

	xkb = 0;
	if (audible_bell) {
		if (XkbQueryExtension(dpy, NULL, &xkb_ev_base, NULL, NULL, NULL)) {
			XkbSelectEvents(dpy, XkbUseCoreKbd,
			    XkbBellNotifyMask, XkbBellNotifyMask);
			xkb_auto_controls = XkbAudibleBellMask;
			xkb_auto_values = XkbAudibleBellMask;
			XkbSetAutoResetControls(dpy,
			    XkbAudibleBellMask, &xkb_auto_controls, &xkb_auto_values);
			XkbChangeEnabledControls(dpy, XkbUseCoreKbd,
			    XkbAudibleBellMask, 0);
			xkb = 1;
		} else
			fprintf(stderr, "Audible bell not suppored by X server\n");
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
				continue;
			}
			if (xkb && xev.type == xkb_ev_base &&
			    ((XkbEvent *)&xev)->any.xkb_type == XkbBellNotify) {
				beep_pending = 1;
				continue;
			}
			if (xev.type != KeyPress)
				continue;
			for (key = key_list; key != NULL; key = key->next) {
				if (xev.xkey.keycode == key->code &&
				    key->map[xev.xkey.state & ShiftMask] == key->sym &&
				    key->modmask == (xev.xkey.state & MODMASK))
					setval(key->name, key->func, key->dir);
			}
		}

		/*
		 * keyboard auto-repeat may try to play beep multiple times,
		 * play it only once
		 */
		if (beep_pending) {
			play_beep();
			beep_pending = 0;
		}

		nfds = 0;
		if (ctl_hdl) {
			ctl_nfds = sioctl_pollfd(ctl_hdl, pfds, 0);
			nfds += ctl_nfds;
		}
		pfds[nfds].fd = ConnectionNumber(dpy);
		pfds[nfds].events = POLLIN;
		nfds++;

		while (poll(pfds, nfds, -1) < 0 && errno == EINTR)
			; /* nothing */

		nfds = 0;
		if (ctl_hdl) {
			if (sioctl_revents(ctl_hdl, pfds + nfds) & POLLHUP) {
				fprintf(stderr, "sndio: hup\n");
				ctl_close();
			}
			nfds += ctl_nfds;
		}

		if (pfds[nfds].revents & POLLHUP) {
			fprintf(stderr, "x11: hup\n");
			break;
		}
		nfds++;
	}

	ungrab_keys();
	XCloseDisplay(dpy);
	maxfds--;

	if (ctl_hdl)
		ctl_close();

	while ((key = key_list) != NULL) {
		key_list = key_list->next;
		free(key);
	}

	return 0;
}
