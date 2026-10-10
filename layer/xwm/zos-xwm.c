/*
 * zos-xwm: the window manager inside the Linux layer's Xwayland.
 *
 * layerrun runs Xwayland full screen on Zurbax's display (rootful), so X11
 * programs live in that one big X screen, where nobody arranges their
 * windows unless a window manager does. This one is made for Steam and
 * games, with no decorations:
 *
 *   - a window that asks for full screen (EWMH _NET_WM_STATE_FULLSCREEN,
 *     before mapping or later) fills the screen: Big Picture and SDL games;
 *   - a resizable main window is maximized (Steam's desktop window), a
 *     fixed-size one or a dialog is centred, over its parent if it has one;
 *   - the newest window and any window clicked get the focus and come to
 *     the front;
 *   - the EWMH and ICCCM properties toolkits look for are kept up to date.
 *
 * Built by layer/build.sh against the layer's libX11; started by layerrun.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { A_WM_STATE, A_WM_PROTOCOLS, A_WM_TAKE_FOCUS, A_WM_CHANGE_STATE, A_NET_SUPPORTED, A_NET_SUPPORTING_WM_CHECK,
       A_NET_WM_NAME, A_UTF8_STRING, A_NET_WM_STATE, A_NET_WM_STATE_FULLSCREEN, A_NET_WM_STATE_MAXIMIZED_VERT,
       A_NET_WM_STATE_MAXIMIZED_HORZ, A_NET_WM_STATE_FOCUSED, A_NET_ACTIVE_WINDOW, A_NET_CLIENT_LIST,
       A_NET_CLIENT_LIST_STACKING, A_NET_WM_WINDOW_TYPE, A_NET_WM_WINDOW_TYPE_DIALOG, A_NET_WM_WINDOW_TYPE_UTILITY,
       A_NET_WM_WINDOW_TYPE_SPLASH, A_NET_WM_WINDOW_TYPE_NORMAL, A_NET_NUMBER_OF_DESKTOPS, A_NET_CURRENT_DESKTOP,
       A_NET_DESKTOP_GEOMETRY, A_NET_WORKAREA, A_NET_WM_DESKTOP, A_NET_CLOSE_WINDOW, A_NET_WM_PID, A_COUNT };

static const char *const atom_names[A_COUNT] = {
	"WM_STATE", "WM_PROTOCOLS", "WM_TAKE_FOCUS", "WM_CHANGE_STATE", "_NET_SUPPORTED", "_NET_SUPPORTING_WM_CHECK",
	"_NET_WM_NAME", "UTF8_STRING", "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_MAXIMIZED_VERT",
	"_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_FOCUSED", "_NET_ACTIVE_WINDOW", "_NET_CLIENT_LIST",
	"_NET_CLIENT_LIST_STACKING", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_UTILITY",
	"_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP",
	"_NET_DESKTOP_GEOMETRY", "_NET_WORKAREA", "_NET_WM_DESKTOP", "_NET_CLOSE_WINDOW", "_NET_WM_PID",
};

typedef struct Client {
	Window w;
	int x, y, width, height;        /* where it is */
	int saved_x, saved_y, saved_w, saved_h; /* where it was before full screen */
	int fullscreen, maximized;
	struct Client *next;            /* front to back */
} Client;

static Display *dpy;
static Window root, check;
static int screen_w, screen_h;
static Atom atoms[A_COUNT];
static Client *clients;

static int on_error(Display *d, XErrorEvent *e)
{
	(void)d, (void)e; /* windows go away between requests; that's normal */
	return 0;
}

static int other_wm;
static int on_startup_error(Display *d, XErrorEvent *e)
{
	(void)d;
	if (e->error_code == BadAccess)
		other_wm = 1;
	return 0;
}

static Client *find(Window w)
{
	for (Client *c = clients; c; c = c->next)
		if (c->w == w)
			return c;
	return NULL;
}

static void update_lists(void)
{
	int n = 0;
	for (Client *c = clients; c; c = c->next)
		n++;
	Window *list = calloc(n ? (size_t)n : 1, sizeof *list);
	/* _NET_CLIENT_LIST_STACKING goes bottom to top */
	int i = n;
	for (Client *c = clients; c; c = c->next)
		list[--i] = c->w;
	XChangeProperty(dpy, root, atoms[A_NET_CLIENT_LIST], XA_WINDOW, 32, PropModeReplace, (unsigned char *)list, n);
	XChangeProperty(dpy, root, atoms[A_NET_CLIENT_LIST_STACKING], XA_WINDOW, 32, PropModeReplace, (unsigned char *)list, n);
	free(list);
}

static void set_wm_state(Window w, long state)
{
	long data[2] = { state, None };
	XChangeProperty(dpy, w, atoms[A_WM_STATE], atoms[A_WM_STATE], 32, PropModeReplace, (unsigned char *)data, 2);
}

static int has_protocol(Window w, Atom proto)
{
	Atom *protos;
	int n, found = 0;
	if (XGetWMProtocols(dpy, w, &protos, &n)) {
		for (int i = 0; i < n; i++)
			found |= protos[i] == proto;
		XFree(protos);
	}
	return found;
}

static void send_protocol(Window w, Atom proto)
{
	XEvent e = { 0 };
	e.xclient.type = ClientMessage;
	e.xclient.window = w;
	e.xclient.message_type = atoms[A_WM_PROTOCOLS];
	e.xclient.format = 32;
	e.xclient.data.l[0] = (long)proto;
	e.xclient.data.l[1] = CurrentTime;
	XSendEvent(dpy, w, False, NoEventMask, &e);
}

/* The window's _NET_WM_STATE, as it reflects what this manager did. */
static void write_net_state(Client *c, int focused)
{
	Atom st[4];
	int n = 0;
	if (c->fullscreen)
		st[n++] = atoms[A_NET_WM_STATE_FULLSCREEN];
	if (c->maximized) {
		st[n++] = atoms[A_NET_WM_STATE_MAXIMIZED_VERT];
		st[n++] = atoms[A_NET_WM_STATE_MAXIMIZED_HORZ];
	}
	if (focused)
		st[n++] = atoms[A_NET_WM_STATE_FOCUSED];
	XChangeProperty(dpy, c->w, atoms[A_NET_WM_STATE], XA_ATOM, 32, PropModeReplace, (unsigned char *)st, n);
}

static void move_resize(Client *c, int x, int y, int w, int h)
{
	c->x = x, c->y = y, c->width = w > 0 ? w : 1, c->height = h > 0 ? h : 1;
	XMoveResizeWindow(dpy, c->w, c->x, c->y, (unsigned)c->width, (unsigned)c->height);
}

static void focus(Client *c)
{
	if (!c) {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, atoms[A_NET_ACTIVE_WINDOW]);
		return;
	}
	/* to the front of the list and the stack */
	Client **pp = &clients;
	while (*pp && *pp != c)
		pp = &(*pp)->next;
	if (*pp) {
		*pp = c->next;
		c->next = clients;
		clients = c;
	}
	XRaiseWindow(dpy, c->w);
	XWMHints *hints = XGetWMHints(dpy, c->w);
	int accepts = !hints || !(hints->flags & InputHint) || hints->input;
	if (hints)
		XFree(hints);
	if (accepts)
		XSetInputFocus(dpy, c->w, RevertToPointerRoot, CurrentTime);
	if (has_protocol(c->w, atoms[A_WM_TAKE_FOCUS]))
		send_protocol(c->w, atoms[A_WM_TAKE_FOCUS]);
	XChangeProperty(dpy, root, atoms[A_NET_ACTIVE_WINDOW], XA_WINDOW, 32, PropModeReplace, (unsigned char *)&c->w, 1);
	for (Client *o = clients; o; o = o->next)
		write_net_state(o, o == c);
	update_lists();
}

static void set_fullscreen(Client *c, int on)
{
	if (on == c->fullscreen)
		return;
	if (on) {
		c->saved_x = c->x, c->saved_y = c->y, c->saved_w = c->width, c->saved_h = c->height;
		c->fullscreen = 1;
		move_resize(c, 0, 0, screen_w, screen_h);
	} else {
		c->fullscreen = 0;
		move_resize(c, c->saved_x, c->saved_y, c->saved_w, c->saved_h);
	}
	XRaiseWindow(dpy, c->w);
	write_net_state(c, clients == c);
}

static int has_atom(Window w, Atom prop, Atom value)
{
	Atom type;
	int format, found = 0;
	unsigned long n, after;
	unsigned char *data = NULL;
	if (XGetWindowProperty(dpy, w, prop, 0, 64, False, XA_ATOM, &type, &format, &n, &after, &data) == Success && data) {
		for (unsigned long i = 0; i < n; i++)
			found |= ((Atom *)data)[i] == value;
		XFree(data);
	}
	return found;
}

/* Where a new window goes: full screen, maximized, or centred (over its parent). */
static void place(Client *c)
{
	XSizeHints hints;
	long supplied;
	int fixed = 0;
	if (XGetWMNormalHints(dpy, c->w, &hints, &supplied) && (hints.flags & PMinSize) && (hints.flags & PMaxSize))
		fixed = hints.min_width == hints.max_width && hints.min_height == hints.max_height;
	Window parent = None;
	XGetTransientForHint(dpy, c->w, &parent);
	int dialog = parent != None || has_atom(c->w, atoms[A_NET_WM_WINDOW_TYPE], atoms[A_NET_WM_WINDOW_TYPE_DIALOG]) ||
		     has_atom(c->w, atoms[A_NET_WM_WINDOW_TYPE], atoms[A_NET_WM_WINDOW_TYPE_UTILITY]) ||
		     has_atom(c->w, atoms[A_NET_WM_WINDOW_TYPE], atoms[A_NET_WM_WINDOW_TYPE_SPLASH]);
	if (has_atom(c->w, atoms[A_NET_WM_STATE], atoms[A_NET_WM_STATE_FULLSCREEN]) ||
	    (c->width >= screen_w && c->height >= screen_h)) {
		c->saved_x = (screen_w - c->width) / 2, c->saved_y = (screen_h - c->height) / 2;
		c->saved_w = c->width, c->saved_h = c->height;
		c->fullscreen = 1;
		move_resize(c, 0, 0, screen_w, screen_h);
		return;
	}
	if (!dialog && !fixed) {
		c->maximized = 1;
		move_resize(c, 0, 0, screen_w, screen_h);
		return;
	}
	/* centred over the parent, or the screen; never off it */
	int cx = screen_w / 2, cy = screen_h / 2;
	Client *p = parent != None ? find(parent) : NULL;
	if (p)
		cx = p->x + p->width / 2, cy = p->y + p->height / 2;
	int w = c->width < screen_w ? c->width : screen_w, h = c->height < screen_h ? c->height : screen_h;
	int x = cx - w / 2, y = cy - h / 2;
	x = x < 0 ? 0 : x + w > screen_w ? screen_w - w : x;
	y = y < 0 ? 0 : y + h > screen_h ? screen_h - h : y;
	move_resize(c, x, y, w, h);
}

static void manage(Window w)
{
	XWindowAttributes wa;
	if (find(w) || !XGetWindowAttributes(dpy, w, &wa) || wa.override_redirect)
		return;
	Client *c = calloc(1, sizeof *c);
	c->w = w, c->x = wa.x, c->y = wa.y, c->width = wa.width, c->height = wa.height;
	c->next = clients;
	clients = c;
	XSetWindowBorderWidth(dpy, w, 0);
	XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);
	/* clicks pass through to the window, after giving it the focus */
	XGrabButton(dpy, AnyButton, AnyModifier, w, False, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
	place(c);
	long desktop = 0;
	XChangeProperty(dpy, w, atoms[A_NET_WM_DESKTOP], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&desktop, 1);
	set_wm_state(w, NormalState);
	XMapWindow(dpy, w);
	focus(c);
}

static void unmanage(Window w)
{
	Client **pp = &clients;
	while (*pp && (*pp)->w != w)
		pp = &(*pp)->next;
	if (!*pp)
		return;
	Client *c = *pp;
	*pp = c->next;
	free(c);
	focus(clients);
}

static void on_client_message(XClientMessageEvent *e)
{
	Client *c = find(e->window);
	if (e->message_type == atoms[A_NET_WM_STATE] && c) {
		/* data: action (0 remove, 1 add, 2 toggle), first property, second property */
		for (int i = 1; i <= 2; i++) {
			if ((Atom)e->data.l[i] != atoms[A_NET_WM_STATE_FULLSCREEN])
				continue;
			long action = e->data.l[0];
			set_fullscreen(c, action == 1 || (action == 2 && !c->fullscreen));
		}
	} else if (e->message_type == atoms[A_NET_ACTIVE_WINDOW] && c) {
		focus(c);
	} else if (e->message_type == atoms[A_NET_CLOSE_WINDOW] && c) {
		Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
		if (has_protocol(c->w, del))
			send_protocol(c->w, del);
		else
			XKillClient(dpy, c->w);
	}
}

static void on_configure_request(XConfigureRequestEvent *e)
{
	Client *c = find(e->window);
	if (c && (c->fullscreen || c->maximized)) {
		/* it keeps the screen; tell it so */
		XConfigureEvent ce = { .type = ConfigureNotify, .display = dpy, .event = c->w, .window = c->w, .x = c->x,
				       .y = c->y, .width = c->width, .height = c->height };
		XSendEvent(dpy, c->w, False, StructureNotifyMask, (XEvent *)&ce);
		return;
	}
	XWindowChanges wc = { .x = e->x, .y = e->y, .width = e->width, .height = e->height, .border_width = 0,
			      .sibling = e->above, .stack_mode = e->detail };
	XConfigureWindow(dpy, e->window, (unsigned)e->value_mask, &wc);
	if (c) {
		if (e->value_mask & CWX)
			c->x = e->x;
		if (e->value_mask & CWY)
			c->y = e->y;
		if (e->value_mask & CWWidth)
			c->width = e->width;
		if (e->value_mask & CWHeight)
			c->height = e->height;
	}
}

static void setup_ewmh(void)
{
	check = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, check, atoms[A_NET_SUPPORTING_WM_CHECK], XA_WINDOW, 32, PropModeReplace, (unsigned char *)&check, 1);
	XChangeProperty(dpy, check, atoms[A_NET_WM_NAME], atoms[A_UTF8_STRING], 8, PropModeReplace, (unsigned char *)"Zurbax", 3);
	XChangeProperty(dpy, root, atoms[A_NET_SUPPORTING_WM_CHECK], XA_WINDOW, 32, PropModeReplace, (unsigned char *)&check, 1);
	Atom supported[] = { atoms[A_NET_SUPPORTED], atoms[A_NET_SUPPORTING_WM_CHECK], atoms[A_NET_WM_NAME],
			     atoms[A_NET_WM_STATE], atoms[A_NET_WM_STATE_FULLSCREEN], atoms[A_NET_WM_STATE_MAXIMIZED_VERT],
			     atoms[A_NET_WM_STATE_MAXIMIZED_HORZ], atoms[A_NET_WM_STATE_FOCUSED], atoms[A_NET_ACTIVE_WINDOW],
			     atoms[A_NET_CLIENT_LIST], atoms[A_NET_CLIENT_LIST_STACKING], atoms[A_NET_WM_WINDOW_TYPE],
			     atoms[A_NET_NUMBER_OF_DESKTOPS], atoms[A_NET_CURRENT_DESKTOP], atoms[A_NET_DESKTOP_GEOMETRY],
			     atoms[A_NET_WORKAREA], atoms[A_NET_WM_DESKTOP], atoms[A_NET_CLOSE_WINDOW] };
	XChangeProperty(dpy, root, atoms[A_NET_SUPPORTED], XA_ATOM, 32, PropModeReplace, (unsigned char *)supported,
			(int)(sizeof supported / sizeof supported[0]));
	long one = 1, zero = 0, geometry[2] = { screen_w, screen_h }, area[4] = { 0, 0, screen_w, screen_h };
	XChangeProperty(dpy, root, atoms[A_NET_NUMBER_OF_DESKTOPS], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&one, 1);
	XChangeProperty(dpy, root, atoms[A_NET_CURRENT_DESKTOP], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&zero, 1);
	XChangeProperty(dpy, root, atoms[A_NET_DESKTOP_GEOMETRY], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)geometry, 2);
	XChangeProperty(dpy, root, atoms[A_NET_WORKAREA], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)area, 4);
}

int main(void)
{
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "zos-xwm: no X display\n");
		return 1;
	}
	root = DefaultRootWindow(dpy);
	screen_w = DisplayWidth(dpy, DefaultScreen(dpy));
	screen_h = DisplayHeight(dpy, DefaultScreen(dpy));
	XSetErrorHandler(on_startup_error);
	XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | StructureNotifyMask);
	XSync(dpy, False);
	if (other_wm) {
		fprintf(stderr, "zos-xwm: another window manager is running\n");
		return 1;
	}
	XSetErrorHandler(on_error);
	XInternAtoms(dpy, (char **)atom_names, A_COUNT, False, atoms);
	setup_ewmh();
	/* windows that were there first */
	Window r, p, *kids;
	unsigned n;
	if (XQueryTree(dpy, root, &r, &p, &kids, &n)) {
		for (unsigned i = 0; i < n; i++) {
			XWindowAttributes wa;
			if (kids[i] != check && XGetWindowAttributes(dpy, kids[i], &wa) && !wa.override_redirect &&
			    wa.map_state == IsViewable)
				manage(kids[i]);
		}
		if (kids)
			XFree(kids);
	}
	for (;;) {
		XEvent e;
		XNextEvent(dpy, &e);
		switch (e.type) {
		case MapRequest:
			manage(e.xmaprequest.window);
			break;
		case UnmapNotify:
			if (e.xunmap.event == root || e.xunmap.send_event)
				break;
			set_wm_state(e.xunmap.window, WithdrawnState);
			unmanage(e.xunmap.window);
			break;
		case DestroyNotify:
			unmanage(e.xdestroywindow.window);
			break;
		case ConfigureRequest:
			on_configure_request(&e.xconfigurerequest);
			break;
		case ClientMessage:
			on_client_message(&e.xclient);
			break;
		case ButtonPress: {
			Client *c = find(e.xbutton.window);
			if (c && c != clients)
				focus(c);
			XAllowEvents(dpy, ReplayPointer, CurrentTime);
			break;
		}
		case ConfigureNotify:
			/* the screen changed size: full-screen windows follow */
			if (e.xconfigure.window == root && (e.xconfigure.width != screen_w || e.xconfigure.height != screen_h)) {
				screen_w = e.xconfigure.width, screen_h = e.xconfigure.height;
				for (Client *c = clients; c; c = c->next)
					if (c->fullscreen || c->maximized)
						move_resize(c, 0, 0, screen_w, screen_h);
			}
			break;
		}
	}
}
