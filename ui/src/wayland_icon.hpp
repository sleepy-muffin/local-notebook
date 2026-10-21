#pragma once

class QApplication;

namespace ui {

// Gives every top-level window the application icon through the compositor's
// xdg-toplevel-icon-v1 protocol.
//
// Qt 6.8's Wayland platform plugin does not implement that protocol, and a
// Wayland client has no other way to hand a window icon to the compositor:
// QApplication::setWindowIcon only reaches Qt's own client-side decorations.
// Without an icon, Plasma falls back to its generic window icon. This talks to
// the compositor directly instead.
//
// A no-op unless the platform plugin in use is Wayland, the compositor
// advertises the protocol, and the build found libwayland-client. Must be
// called after QApplication exists and before the first window is shown.
void install_wayland_toplevel_icon(QApplication& application);

} // namespace ui
