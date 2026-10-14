// Torchlight's Cinnamon extension (ADR 0054), the Cinnamon port of its GNOME Shell extension
// (ADR 0051). Cinnamon on Wayland does not let applications see other applications' windows,
// so the Torchlight launcher asks Cinnamon, over the session bus, for the open windows (most
// recently used first) and to switch to one.
// Interface org.torchlight.Windows at /org/torchlight/Windows on the org.Cinnamon name:
//   List() -> a(tssss): window id, WM_CLASS instance, WM_CLASS class or Wayland app id,
//                       the window's application id (its .desktop file name without
//                       ".desktop", or the GTK application id), title
//   Activate(t id) -> b: whether the window was still open
// Any process of the same user can call it, as X11 always allowed; sandboxed applications
// cannot reach Cinnamon's bus name.
const Gio = imports.gi.Gio;
const Meta = imports.gi.Meta;
const Cinnamon = imports.gi.Cinnamon;
const Main = imports.ui.main;

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

// The desktop file a window belongs to, as Cinnamon matched it, without ".desktop"; windows
// Cinnamon could not match fall back to their GTK application id.
function applicationId(tracker, window) {
    const app = tracker.get_window_app(window);
    if (app && app.get_app_info())
        return app.get_id().replace(DESKTOP_SUFFIX, '');
    return window.get_gtk_application_id() || '';
}

class WindowsService {
    get Version() {
        return 1;
    }

    List() {
        const tracker = Cinnamon.WindowTracker.get_default();
        // The tab list holds normal windows on every workspace, most recently used first.
        return global.display.get_tab_list(Meta.TabList.NORMAL_ALL, null).map(window => [
            window.get_id(),
            window.get_wm_class_instance() || '',
            window.get_wm_class() || '',
            applicationId(tracker, window),
            window.get_title() || '',
        ]);
    }

    Activate(id) {
        // Cinnamon's window manager has no list_all_windows; every window has an actor.
        const window = global.get_window_actors()
            .map(actor => actor.meta_window)
            .find(candidate => candidate.get_id() === id);
        if (!window)
            return false;
        // Switches to the window's workspace first, as the window list applet does.
        const workspace = window.get_workspace();
        Main.activateWindow(window, global.get_current_time(),
            workspace ? workspace.index() : undefined);
        return true;
    }
}

let service = null;

function init(metadata) {
}

function enable() {
    service = Gio.DBusExportedObject.wrapJSObject(INTERFACE, new WindowsService());
    service.export(Gio.DBus.session, OBJECT_PATH);
}

function disable() {
    if (service)
        service.unexport();
    service = null;
}
