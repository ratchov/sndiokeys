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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sndio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

/*
 * number of level steps between 0 and 1
 */
#define NSTEP		20

static void inc_level(void);
static void dec_level(void);
static void cycle_dev(void);

struct modname {
	unsigned int mask;
	char *name;
} modname_tab[] = {
	{ControlMask, "Control"},
	{Mod1Mask, "Mod1"},
	{Mod2Mask, "Mod2"},
	{Mod3Mask, "Mod3"},
	{Mod4Mask, "Mod4"},
	{Mod5Mask, "Mod5"},
	{0, NULL}
};

struct funcname {
	void (*func)(void);
	char *name;
} funcname_tab[] = {
	{inc_level, "inc_level"},
	{dec_level, "dec_level"},
	{cycle_dev, "cycle_dev"},
	{NULL, NULL}
};

struct ctl {
	struct ctl *next;
	struct sioctl_desc desc;
	int val;
} *ctl_list;

struct key {
	struct key *next;
	char *name;
	unsigned int modmask;
	KeySym sym;
	KeyCode code;
	KeySym *map;
	void (*func)(void);
} *key_list;

Display	*dpy;
struct sioctl_hdl *hdl;
char *dev_name;
int verbose;
int silent;

static void
play_beep(void)
{
#define BELL_RATE	48000
#define BELL_LEN	(BELL_RATE / 20)
#define BELL_PERIOD	(BELL_RATE / 880)
#define BELL_AMP	(INT16_MAX / 32)
	int16_t data[BELL_LEN];
	struct sio_hdl *hdl;
	struct sio_par par;
	int i;

	hdl = sio_open(dev_name, SIO_PLAY, 0);
	if (hdl == NULL) {
		if (verbose)
			fprintf(stderr, "bell: failed to open audio device\n");
		return;
	}

	sio_initpar(&par);
	par.bits = 16;
	par.rate = BELL_RATE;
	par.pchan = 1;

	if (!sio_setpar(hdl, &par) || !sio_getpar(hdl, &par)) {
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

	if (!sio_start(hdl)) {
		if (verbose)
			fprintf(stderr, "bell: failed to start playback\n");
		goto err_close;
	}

	for (i = 0; i < BELL_LEN; i++) {
		data[i] = (i % BELL_PERIOD) < (BELL_PERIOD / 2) ?
		    BELL_AMP : -BELL_AMP;
	}

	if (sio_write(hdl, data, sizeof(data)) != sizeof(data)) {
		if (verbose)
			fprintf(stderr, "bell: short write\n");
		goto err_close;
	}

err_close:
	sio_close(hdl);
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
 * sndio call-back for control value/changes
 */
static void
onval(void *unused, unsigned int addr, unsigned int val)
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
 * change output.level control by given increment
 */
static void
change_level(int dir)
{
	int incr, vol;
	struct ctl *i;

	for (i = ctl_list; i != NULL; i = i->next) {
		if (i->desc.group[0] != 0 ||
		    strcmp(i->desc.node0.name, "output") != 0 ||
		    strcmp(i->desc.func, "level") != 0)
			continue;

		incr = ((int)i->desc.maxval + NSTEP - 1) / NSTEP;
		vol = i->val + dir * incr;
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
 * increase output.level
 */
static void
inc_level(void)
{
	change_level(1);
}

/*
 * increase output.level
 */
static void
dec_level(void)
{
	change_level(-1);
}

/*
 * cycle server.device
 */
static void
cycle_dev(void)
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

	if (!silent)
		play_beep();
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

		key->sym = XStringToKeysym(key->name);
		if (key->sym == NoSymbol) {
			fprintf(stderr, "%s: couldn't find keysym for key\n", key->name);
			exit(1);
		}
		key->code = XKeysymToKeycode(dpy, key->sym);
		key->map = XGetKeyboardMapping(dpy, key->code, 1, &nret);
		if (nret <= ShiftMask) {
			fprintf(stderr, "%s: couldn't get keymap for key\n", key->name);
			exit(1);
		}

		nscr = ScreenCount(dpy);
		for (i = 0; i <= 0xff; i++) {
			if ((i & key->modmask) != 0)
				continue;
			for (scr = 0; scr != nscr; scr++) {
				XGrabKey(dpy, key->code, i | key->modmask,
				    RootWindow(dpy, scr), 1,
				    GrabModeAsync, GrabModeAsync);
			}
		}
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
void
add_key(unsigned int modmask, char *name, void (*func)(void))
{
	struct key *key, **p;

	/* delete bindings for the same function */
	p = &key_list;
	while ((key = *p) != NULL) {
		if (key->func == func) {
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
	key->name = name;
	key->modmask = modmask;
	key->func = func;

	key->next = NULL;
	*p = key;
}


void
parsekey(char *str)
{
	char *p, *end;
	struct modname *mod;
	struct funcname *func;
	char *keysym;
	unsigned int modmask;

	p = str;

	modmask = 0;
	while (1) {
		end = strchr(p, '+');
		if (end == NULL)
			break;
		*end = 0;

		mod = modname_tab;
		while (1) {
			if (mod->mask == 0) {
				fprintf(stderr, "%s: bad modifiers\n", str);
				exit(1);
			}
			if (strcmp(p, mod->name) == 0) {
				modmask |= mod->mask;
				break;
			}
			mod++;
		}

		p = end + 1;
	}

	end = strchr(p, ':');
	if (end == NULL) {
		fprintf(stderr, "%s: expected ':'\n", str);
		exit(1);
	}
	*end = 0;

	/* no need to strdup() */
	keysym = p;

	/* skip ':' */
	p = end + 1;

	func = funcname_tab;
	while (1) {
		if (func->func == NULL) {
			fprintf(stderr, "%s: bad function name\n", str);
			exit(1);
		}
		if (strcmp(p, func->name) == 0)
			break;

		func++;
	}

	add_key(modmask, keysym, func->func);
}

int
main(int argc, char **argv)
{
	int scr;
	XEvent xev;
	int c, nfds;
	int background;
	struct ctl *i;
	struct pollfd *pfds;
	struct key *key;

	dev_name = SIO_DEVANY;
	verbose = 0;
	background = 0;
	add_key(ControlMask | Mod1Mask, "plus", inc_level);
	add_key(ControlMask | Mod1Mask, "minus", dec_level);
	add_key(ControlMask | Mod1Mask, "0", cycle_dev);

	while ((c = getopt(argc, argv, "b:Df:m:sv")) != -1) {
		switch (c) {
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
		    "[-Dsv] "
		    "[-b [[mod+...]key:func] "
		    "[-f device]\n",
		    stderr);
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
	sioctl_ondesc(hdl, ondesc, NULL);
	sioctl_onval(hdl, onval, NULL);

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
			for (key = key_list; key != NULL; key = key->next) {
				if (xev.xkey.keycode == key->code &&
				    key->map[xev.xkey.state & ShiftMask] == key->sym)
					key->func();
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

	ungrab_keys();
	XCloseDisplay(dpy);

	sioctl_close(hdl);

	while ((i = ctl_list) != NULL) {
		ctl_list = ctl_list->next;
		free(i);
	}

	while ((key = key_list) != NULL) {
		key_list = key_list->next;
		free(key);
	}

	return 0;
}
