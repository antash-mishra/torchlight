// Torchlight's GNOME Shell extension (ADR 0051). GNOME on Wayland does not let applications
// see other applications' windows, so the Torchlight launcher asks the shell, over the
// session bus, for the open windows (most recently used first) and to switch to one.
// Interface org.torchlight.Windows at /org/torchlight/Windows on the org.gnome.Shell name:
//   List() -> a(tssss): window id, WM_CLASS instance, WM_CLASS class or Wayland app id,
//                       the window's application id (its .desktop file name without
//                       ".desktop", or the GTK application id), title
//   Activate(t id) -> b: whether the window was still open
// Any process of the same user can call it, as X11 always allowed; sandboxed applications
// cannot reach the shell's bus name.
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const OBJECT_PATH = '/org/torchlight/Windows';
const INTERFACE = `<node>
  <interface name="org.torchlight.Windows">
    <method name="List">
      <arg type="a(tssss)" direction="out" name="windows"/>
    </method>
    <method name="Activate">
      <arg type="t" direction="in" name="id"/>
      <arg type="b" direction="out" name="found"/>
    </method>
    <property name="Version" type="u" access="read"/>
  </interface>
</node>`;
const DESKTOP_SUFFIX = /\.desktop$/;

// The desktop file a window belongs to, as GNOME Shell matched it, without ".desktop";
// windows the shell could not match fall back to their GTK application id.
function applicationId(tracker, window) {
    const app = tracker.get_window_app(window);
    if (app?.get_app_info())
        return app.get_id().replace(DESKTOP_SUFFIX, '');
    return window.get_gtk_application_id() ?? '';
}

class WindowsService {
    get Version() {
        return 1;
    }

    List() {
        const tracker = Shell.WindowTracker.get_default();
        // The tab list holds normal windows on every workspace, most recently used first.
        return global.display.get_tab_list(Meta.TabList.NORMAL_ALL, null).map(window => [
            window.get_id(),
            window.get_wm_class_instance() ?? '',
            window.get_wm_class() ?? '',
            applicationId(tracker, window),
            window.get_title() ?? '',
        ]);
    }

    Activate(id) {
        const window = global.display.list_all_windows().find(candidate => candidate.get_id() === id);
        if (!window)
            return false;
        // Switches to the window's workspace and focuses it, as the overview does.
        Main.activateWindow(window);
        return true;
    }
}

export default class TorchlightExtension extends Extension {
    enable() {
        this._service = Gio.DBusExportedObject.wrapJSObject(INTERFACE, new WindowsService());
        this._service.export(Gio.DBus.session, OBJECT_PATH);
    }

    disable() {
        this._service?.unexport();
        this._service = null;
    }
}
