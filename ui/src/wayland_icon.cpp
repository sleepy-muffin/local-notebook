#include "wayland_icon.hpp"

#include <QGuiApplication>

// QNativeInterface::QWaylandApplication arrived in Qt 6.5, and ui/CMakeLists.txt
// only defines LNB_HAVE_WAYLAND_ICON when it is available. This checks the
// version directly rather than QT_CONFIG(wayland): older qtgui has no "wayland"
// feature, and QT_CONFIG() evaluates an undefined feature as a division by
// zero, which is a hard preprocessor error.
#if defined(LNB_HAVE_WAYLAND_ICON) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)

#include <QApplication>
#include <QEvent>
#include <QIcon>
#include <QImage>
#include <QMargins>
#include <QPixmap>
#include <QPointer>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <any>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wayland-client.h>

#include <QtGui/qguiapplication_platform.h>

#include "shark/log.hpp"
#include "ui/theme.hpp"
#include "wayland/xdg-toplevel-icon-v1-client-protocol.h"

// The generated private-code references the toplevel interface so libwayland
// can describe the set_icon request. Qt's xdg-shell implementation lives in a
// dlopen'ed plugin and does not export it; libwayland-client only consults the
// descriptor for message metadata (the compositor validates the object it
// receives), so an empty descriptor is enough and keeps the generated files
// untouched. It stays hidden so it can never shadow the plugin's own one.
extern "C" __attribute__((visibility("hidden")))
const struct wl_interface xdg_toplevel_interface = {
    "xdg_toplevel", 1, 0, nullptr, 0, nullptr};

namespace {

// Qt's Wayland platform plugin exposes its per-window object through this
// native interface, which is the only way to reach a toplevel's xdg_toplevel
// proxy from outside QtWayland. The declaration lives in Qt's private
// qplatformwindow_p.h, which not every distribution installs, so the fragment
// needed here is mirrored instead. Qt guarantees binary compatibility within
// Qt 6, and these virtuals keep their order across 6.x releases, so the vtable
// slots stay valid. The interface itself is resolved by name at runtime, and
// is only queried once the platform has been confirmed to be Wayland.
struct QWaylandWindow : public QObject {
  QT_DECLARE_NATIVE_INTERFACE(QWaylandWindow, 1, QWindow)

  virtual wl_surface* surface() const = 0;
  virtual void setCustomMargins(const QMargins& margins) = 0;
  virtual void requestXdgActivationToken(uint serial) = 0;

  template <typename T>
  T* surfaceRole() const {
    std::any anyRole = _surfaceRole();
    auto role = std::any_cast<T*>(&anyRole);
    return role ? *role : nullptr;
  }

protected:
  virtual std::any _surfaceRole() const = 0;
};

// One embedded icon size, uploaded to shared memory once and reused for every
// toplevel. The protocol requires the buffer to stay alive for as long as the
// icon referencing it, so the buffers (a few megabytes at most) live for the
// process lifetime.
struct ShmImage {
  wl_buffer* buffer = nullptr;
  void* mapping = nullptr;
  std::size_t size = 0;
  int scale = 1;
};

struct IconState {
  wl_display* display = nullptr;
  wl_shm* shm = nullptr;
  xdg_toplevel_icon_manager_v1* manager = nullptr;
  std::vector<ShmImage> images;
  // Icons are immutable once assigned to a toplevel, so one is created per
  // window. They are kept alive rather than destroyed immediately after
  // set_icon, so no compositor can observe a dangling icon.
  std::vector<xdg_toplevel_icon_v1*> icons;
};

void shm_format(void*, wl_shm*, uint32_t) {}
const wl_shm_listener shm_listener = {shm_format};

void manager_icon_size(void*, xdg_toplevel_icon_manager_v1*, int32_t) {}
void manager_done(void*, xdg_toplevel_icon_manager_v1*) {}
const xdg_toplevel_icon_manager_v1_listener manager_listener = {
    manager_icon_size,
    manager_done,
};

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t version) {
  auto& state = *static_cast<IconState*>(data);

  if (std::strcmp(interface, xdg_toplevel_icon_manager_v1_interface.name) == 0) {
    state.manager = static_cast<xdg_toplevel_icon_manager_v1*>(
        wl_registry_bind(registry, name, &xdg_toplevel_icon_manager_v1_interface,
                         std::min(version, 1u)));
    xdg_toplevel_icon_manager_v1_add_listener(state.manager, &manager_listener,
                                              &state);
  } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
    state.shm = static_cast<wl_shm*>(
        wl_registry_bind(registry, name, &wl_shm_interface, 1));
    wl_shm_add_listener(state.shm, &shm_listener, &state);
  }
}

void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};

// An anonymous file that backs one wl_shm buffer. memfd_create covers Linux;
// the POSIX fallback is unlinked immediately, so it disappears with the last
// descriptor (and the buffer mapped from it).
int create_shm_fd(std::size_t size) {
#if defined(MFD_CLOEXEC)
  const int memory_fd = memfd_create("local-notebook-icon", MFD_CLOEXEC);

  if (memory_fd >= 0) {
    if (ftruncate(memory_fd, static_cast<off_t>(size)) == 0) {
      return memory_fd;
    }

    close(memory_fd);
  }
#endif

  for (int attempt = 0; attempt < 16; ++attempt) {
    char name[64];
    std::snprintf(name, sizeof name, "/local-notebook-icon-%ld-%d",
                  static_cast<long>(getpid()), attempt);

    const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);

    if (fd < 0) {
      if (errno == EEXIST) {
        continue;
      }

      return -1;
    }

    shm_unlink(name);

    if (ftruncate(fd, static_cast<off_t>(size)) == 0) {
      return fd;
    }

    close(fd);
    return -1;
  }

  return -1;
}

// Copies one embedded icon into a wl_shm buffer. XDG icons are always square
// and the protocol rejects anything else, so other shapes are skipped. The
// buffer holds the pixmap's physical pixels, and its device pixel ratio is the
// scale the compositor needs to map them back to a logical icon size.
void add_image(IconState& state, const QPixmap& pixmap) {
  if (pixmap.isNull()) {
    return;
  }

  const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
  const int width = image.width();
  const int height = image.height();

  if (width <= 0 || width != height) {
    return;
  }

  const int scale = std::max(1, qRound(pixmap.devicePixelRatio()));
  const int stride = width * 4;
  const std::size_t size =
      static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);

  const int fd = create_shm_fd(size);

  if (fd < 0) {
    shark::log::debug("wayland icon: could not create shared memory for {}px", width);
    return;
  }

  void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (mapping == MAP_FAILED) {
    close(fd);
    return;
  }

  for (int row = 0; row < height; ++row) {
    std::memcpy(static_cast<char*>(mapping) + static_cast<std::size_t>(row) * stride,
                image.constScanLine(row), static_cast<std::size_t>(stride));
  }

  wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int32_t>(size));
  wl_buffer* buffer = wl_shm_pool_create_buffer(
      pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);

  if (buffer == nullptr) {
    munmap(mapping, size);
    return;
  }

  state.images.push_back(ShmImage{buffer, mapping, size, scale});
}

// Uploads every size the bundled QIcon carries, so the compositor can pick a
// crisp one for the taskbar, the window switcher and the overview.
void add_application_images(IconState& state) {
  const QIcon icon = ui::application_icon();

  for (const QSize& size : icon.availableSizes()) {
    add_image(state, icon.pixmap(size));
  }
}

bool may_have_toplevel_role(const QWindow* window) {
  // Only xdg-shell toplevels can carry an icon. Popups (menus, combo boxes,
  // tooltips) get an xdg_popup role instead, which has none.
  switch (window->type()) {
    case Qt::Window:
    case Qt::Dialog:
    case Qt::Sheet:
    case Qt::Drawer:
    case Qt::Tool:
      return true;
    default:
      return false;
  }
}

bool apply_icon(IconState& state, QWindow* window) {
  if (window == nullptr || state.manager == nullptr) {
    return false;
  }

  auto* native = window->nativeInterface<QWaylandWindow>();

  if (native == nullptr) {
    return false;
  }

  wl_surface* surface = native->surface();
  auto* toplevel = native->surfaceRole<xdg_toplevel>();

  // Popups, tooltips and other non-toplevel surfaces have no xdg_toplevel
  // role; the compositor has no icon for them, and that is fine.
  if (surface == nullptr || toplevel == nullptr) {
    return false;
  }

  xdg_toplevel_icon_v1* icon = xdg_toplevel_icon_manager_v1_create_icon(state.manager);

  if (icon == nullptr) {
    return false;
  }

  for (const ShmImage& image : state.images) {
    xdg_toplevel_icon_v1_add_buffer(icon, image.buffer, image.scale);
  }

  xdg_toplevel_icon_manager_v1_set_icon(state.manager, toplevel, icon);
  state.icons.push_back(icon);

  // The icon is double-buffered on the toplevel and only applied on its next
  // commit. Qt owns the surface, so commit it here to make the icon arrive
  // before Qt attaches the window's first frame.
  wl_surface_commit(surface);
  wl_display_flush(state.display);
  return true;
}

class WaylandIconInstaller final : public QObject {
public:
  explicit WaylandIconInstaller(QApplication& application)
      : QObject(&application) {}

  bool initialize(wl_display* display) {
    state_.display = display;

    wl_registry* registry = wl_display_get_registry(display);

    if (registry == nullptr) {
      return false;
    }

    wl_registry_add_listener(registry, &registry_listener, &state_);

    if (wl_display_roundtrip(display) < 0) {
      wl_registry_destroy(registry);
      return false;
    }

    // The bound globals outlive the registry as far as this process is
    // concerned, so the registry itself is no longer needed.
    wl_registry_destroy(registry);

    if (state_.manager == nullptr || state_.shm == nullptr) {
      return false;
    }

    add_application_images(state_);
    return !state_.images.empty();
  }

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::Show) {
      if (auto* window = qobject_cast<QWindow*>(watched);
          window != nullptr && window->isTopLevel()) {
        apply(window);
      }
    }

    return QObject::eventFilter(watched, event);
  }

  void apply(QWindow* window) {
    if (window == nullptr || was_applied(window)) {
      return;
    }

    if (apply_icon(state_, window)) {
      applied_.push_back(QPointer<QWindow>(window));
      return;
    }

    if (!may_have_toplevel_role(window) || was_scheduled(window)) {
      return;
    }

    // Qt creates the xdg_toplevel after the Show event has been delivered (the
    // shell surface is set up when the window is first committed), so try again
    // once the event loop has had a chance to run.
    scheduled_.push_back(QPointer<QWindow>(window));
    const QPointer<QWindow> guard(window);
    QTimer::singleShot(0, this, [this, guard] {
      if (guard != nullptr) {
        apply(guard.data());
      }
    });
  }

private:
  static bool contains(const std::vector<QPointer<QWindow>>& windows,
                       const QWindow* window) {
    return std::any_of(windows.begin(), windows.end(),
                       [window](const QPointer<QWindow>& entry) {
                         return entry.data() == window;
                       });
  }

  bool was_applied(const QWindow* window) const {
    return contains(applied_, window);
  }

  bool was_scheduled(const QWindow* window) const {
    return contains(scheduled_, window);
  }

  IconState state_;
  std::vector<QPointer<QWindow>> applied_;
  std::vector<QPointer<QWindow>> scheduled_;
};

} // namespace

namespace ui {

void install_wayland_toplevel_icon(QApplication& application) {
  if (QGuiApplication::platformName() != QLatin1String("wayland")) {
    return;
  }

  auto* wayland = application.nativeInterface<QNativeInterface::QWaylandApplication>();

  if (wayland == nullptr) {
    return;
  }

  wl_display* display = wayland->display();

  if (display == nullptr) {
    return;
  }

  auto* installer = new WaylandIconInstaller(application);

  if (!installer->initialize(display)) {
    shark::log::debug("wayland icon: xdg-toplevel-icon-v1 unavailable, "
                      "leaving the window icon to Qt");
    delete installer;
    return;
  }

  application.installEventFilter(installer);

  // Windows that were already shown before this call (there should be none at
  // startup) still get the icon; later ones arrive through the event filter.
  for (QWindow* window : QGuiApplication::topLevelWindows()) {
    installer->apply(window);
  }
}

} // namespace ui

#else

namespace ui {

void install_wayland_toplevel_icon(QApplication&) {}

} // namespace ui

#endif
