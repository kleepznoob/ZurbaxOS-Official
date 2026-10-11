// Zurbax Taskbar: the Zurbax desktop's taskbar in GNOME Shell.
//
// A bar along the bottom of the main screen with the app grid button, then
// the favorite apps and the open ones, each with a mark when it's running
// (a longer one, in the accent color, for the app in front). Clicking an
// app brings it forward, starts it, or, when it's already in front, puts
// its windows away; the middle button opens a new window. Windows keep
// clear of the bar.

import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as AppFavorites from 'resource:///org/gnome/shell/ui/appFavorites.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const HEIGHT = 52;
const ICON_SIZE = 32;

function focusApp() {
    return Shell.WindowTracker.get_default().focus_app;
}

const AppButton = GObject.registerClass(
class AppButton extends St.Button {
    _init(app, taskbar) {
        super._init({
            style_class: 'zurbax-taskbar-button',
            reactive: true,
            can_focus: true,
            track_hover: true,
            button_mask: St.ButtonMask.ONE | St.ButtonMask.TWO,
            accessible_name: app.get_name(),
        });
        this.app = app;
        const box = new St.BoxLayout({
            orientation: Clutter.Orientation.VERTICAL,
            x_align: Clutter.ActorAlign.CENTER,
        });
        box.add_child(app.create_icon_texture(ICON_SIZE));
        this._mark = new St.Widget({style_class: 'zurbax-taskbar-mark', x_align: Clutter.ActorAlign.CENTER});
        box.add_child(this._mark);
        this.set_child(box);
        this.connect('clicked', (_button, which) => this._clicked(which));
        this.connect('notify::hover', () => taskbar.tooltip(this));
        this.sync();
    }

    sync() {
        const running = this.app.state !== Shell.AppState.STOPPED;
        const front = running && focusApp() === this.app && !Main.overview.visible;
        this._mark.opacity = running ? 255 : 0;
        for (const actor of [this, this._mark]) {
            if (front)
                actor.add_style_class_name('front');
            else
                actor.remove_style_class_name('front');
        }
    }

    _clicked(which) {
        const inFront = focusApp() === this.app && !Main.overview.visible;
        if (Main.overview.visible)
            Main.overview.hide();
        if (which === Clutter.BUTTON_MIDDLE) {
            if (this.app.can_open_new_window())
                this.app.open_new_window(-1);
            return;
        }
        const windows = this.app.get_windows().filter(w => !w.skip_taskbar);
        if (inFront && windows.length) {
            for (const w of windows) {
                if (w.can_minimize())
                    w.minimize();
            }
        } else {
            this.app.activate();
        }
    }
});

const Taskbar = GObject.registerClass(
class Taskbar extends St.BoxLayout {
    _init() {
        super._init({style_class: 'zurbax-taskbar', name: 'zurbaxTaskbar', reactive: true});

        const grid = new St.Button({
            style_class: 'zurbax-taskbar-button zurbax-taskbar-grid',
            can_focus: true,
            track_hover: true,
            accessible_name: 'Show Apps',
            child: new St.Icon({icon_name: 'view-app-grid-symbolic', icon_size: 22}),
        });
        grid.connect('clicked', () => {
            if (Main.overview.visible && Main.overview.dash?.showAppsButton?.checked)
                Main.overview.hide();
            else
                Main.overview.showApps();
        });
        this.add_child(grid);
        this.add_child(new St.Widget({style_class: 'zurbax-taskbar-separator', y_align: Clutter.ActorAlign.CENTER}));
        this._apps = new St.BoxLayout({style_class: 'zurbax-taskbar-apps', x_expand: true});
        this.add_child(this._apps);

        this._tip = new St.Label({style_class: 'zurbax-taskbar-tooltip', visible: false});
        Main.uiGroup.add_child(this._tip);

        this._buttons = new Map();
        this._rebuildId = 0;
        Shell.AppSystem.get_default().connectObject('app-state-changed', () => this._queueRebuild(), this);
        AppFavorites.getAppFavorites().connectObject('changed', () => this._queueRebuild(), this);
        Shell.WindowTracker.get_default().connectObject('notify::focus-app', () => this._sync(), this);
        Main.overview.connectObject('showing', () => this._sync(), 'hidden', () => this._sync(), this);
        this.connect('destroy', () => this._onDestroy());
        this._rebuild();
    }

    // favorites first, in their order, then whatever else is open
    _rebuild() {
        this._rebuildId = 0;
        const list = [...AppFavorites.getAppFavorites().getFavorites()];
        for (const app of Shell.AppSystem.get_default().get_running()) {
            if (!list.includes(app))
                list.push(app);
        }
        const keep = new Map();
        for (const app of list)
            keep.set(app.get_id(), this._buttons.get(app.get_id()) ?? new AppButton(app, this));
        for (const [id, button] of this._buttons) {
            if (!keep.has(id))
                button.destroy();
        }
        this._buttons = keep;
        let i = 0;
        for (const button of keep.values()) {
            if (button.get_parent() !== this._apps)
                this._apps.insert_child_at_index(button, i);
            else
                this._apps.set_child_at_index(button, i);
            i++;
        }
        this._sync();
        return GLib.SOURCE_REMOVE;
    }

    _queueRebuild() {
        if (!this._rebuildId)
            this._rebuildId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => this._rebuild());
    }

    _sync() {
        for (const button of this._buttons.values())
            button.sync();
    }

    tooltip(button) {
        if (!button.hover) {
            this._tip.hide();
            return;
        }
        this._tip.text = button.app.get_name();
        this._tip.show();
        const [x, y] = button.get_transformed_position();
        const [w] = button.get_transformed_size();
        this._tip.set_position(Math.round(x + w / 2 - this._tip.width / 2), Math.round(y - this._tip.height - 8));
    }

    place() {
        const monitor = Main.layoutManager.primaryMonitor;
        if (!monitor)
            return;
        this.set_position(monitor.x, monitor.y + monitor.height - HEIGHT);
        this.set_size(monitor.width, HEIGHT);
    }

    _onDestroy() {
        if (this._rebuildId)
            GLib.source_remove(this._rebuildId);
        this._tip.destroy();
    }
});

export default class ZurbaxTaskbarExtension extends Extension {
    enable() {
        this._taskbar = new Taskbar();
        Main.layoutManager.addChrome(this._taskbar, {affectsStruts: true, trackFullscreen: true});
        this._taskbar.place();
        Main.layoutManager.connectObject('monitors-changed', () => this._taskbar.place(), this);
    }

    disable() {
        Main.layoutManager.disconnectObject(this);
        this._taskbar.destroy();
        this._taskbar = null;
    }
}
