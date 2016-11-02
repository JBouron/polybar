#include "x11/tray.hpp"
#include "utils/color.hpp"
#include "x11/draw.hpp"
#include "x11/window.hpp"

LEMONBUDDY_NS

// trayclient {{{

trayclient::trayclient(connection& conn, xcb_window_t win, uint16_t w, uint16_t h)
    : m_connection(conn), m_window(win), m_width(w), m_height(h) {
  m_xembed = memory_util::make_malloc_ptr<xembed_data>();
  m_xembed->version = XEMBED_VERSION;
  m_xembed->flags = XEMBED_MAPPED;
}

trayclient::~trayclient() {
  xembed::unembed(m_connection, window(), m_connection.root());
}

/**
 * Match given window against client window
 */
bool trayclient::match(const xcb_window_t& win) const {  // {{{
  return win == m_window;
}  // }}}

/**
 * Get client window mapped state
 */
bool trayclient::mapped() const {  // {{{
  return m_mapped;
}  // }}}

/**
 * Set client window mapped state
 */
void trayclient::mapped(bool state) {  // {{{
  m_mapped = state;
}  // }}}

/**
 * Get client window
 */
xcb_window_t trayclient::window() const {  // {{{
  return m_window;
}  // }}}

/**
 * Get xembed data pointer
 */
xembed_data* trayclient::xembed() const {  // {{{
  return m_xembed.get();
}  // }}}

/**
 * Make sure that the window mapping state is correct
 */
void trayclient::ensure_state() const {  // {{{
  if (!mapped() && ((xembed()->flags & XEMBED_MAPPED) == XEMBED_MAPPED)) {
    m_connection.map_window_checked(window());
  } else if (mapped() && ((xembed()->flags & XEMBED_MAPPED) != XEMBED_MAPPED)) {
    m_connection.unmap_window_checked(window());
  }
}  // }}}

/**
 * Configure window size
 */
void trayclient::reconfigure(int16_t x, int16_t y) const {  // {{{
  uint32_t configure_mask = 0;
  uint32_t configure_values[7];
  xcb_params_configure_window_t configure_params;

  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, width, m_width);
  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, height, m_height);
  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, x, x);
  XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, y, y);

  xutils::pack_values(configure_mask, &configure_params, configure_values);
  m_connection.configure_window_checked(window(), configure_mask, configure_values);
}  // }}}

/**
 * Respond to client resize requests
 */
void trayclient::configure_notify(int16_t x, int16_t y) const {  // {{{
  auto notify = memory_util::make_malloc_ptr<xcb_configure_notify_event_t>(32);
  notify->response_type = XCB_CONFIGURE_NOTIFY;
  notify->event = m_window;
  notify->window = m_window;
  notify->override_redirect = false;
  notify->above_sibling = XCB_NONE;
  notify->x = x;
  notify->y = y;
  notify->width = m_width;
  notify->height = m_height;
  notify->border_width = 0;

  const char* data = reinterpret_cast<const char*>(notify.get());
  m_connection.send_event_checked(false, m_window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, data);
}  // }}}

// }}}
// traymanager {{{

traymanager::traymanager(connection& conn, const logger& logger)
    : m_connection(conn), m_log(logger) {
  m_connection.attach_sink(this, 2);
  m_sinkattached = true;
}

traymanager::~traymanager() {
  if (m_activated)
    deactivate();
  if (m_sinkattached)
    m_connection.detach_sink(this, 2);
}

/**
 * Initialize data
 */
void traymanager::bootstrap(tray_settings settings) {  // {{{
  m_settings = settings;
  query_atom();
}  // }}}

/**
 * Activate systray management
 */
void traymanager::activate() {  // {{{
  if (m_activated) {
    return;
  }

  if (m_tray == XCB_NONE) {
    try {
      create_window();
      set_wmhints();
      set_traycolors();
    } catch (const std::exception& err) {
      m_log.err(err.what());
      m_log.err("Cannot activate traymanager... failed to setup window");
      return;
    }
  }

  m_log.info("Activating traymanager");
  m_activated = true;

  if (!m_sinkattached) {
    m_connection.attach_sink(this, 2);
    m_sinkattached = true;
  }

  // Listen for visibility change events on the bar window
  if (!m_restacked && !g_signals::bar::visibility_change) {
    g_signals::bar::visibility_change =
        bind(&traymanager::bar_visibility_change, this, std::placeholders::_1);
  }

  // Attempt to get control of the systray selection then
  // notify clients waiting for a manager.
  acquire_selection();

  // If replacing an existing manager or if re-activating from getting
  // replaced, we delay the notification broadcast to allow the clients
  // to get unembedded...
  if (m_othermanager != XCB_NONE)
    this_thread::sleep_for(1s);
  notify_clients();

  m_connection.flush();
}  // }}}

/**
 * Deactivate systray management
 */
void traymanager::deactivate() {  // {{{
  if (!m_activated) {
    return;
  }

  m_log.info("Deactivating traymanager");
  m_activated = false;

  if (m_delayed_activation.joinable())
    m_delayed_activation.join();

  if (g_signals::tray::report_slotcount) {
    m_log.trace("tray: Report empty slotcount");
    g_signals::tray::report_slotcount(0);
  }

  if (g_signals::bar::visibility_change) {
    m_log.trace("tray: Clear callback handlers");
    g_signals::bar::visibility_change = nullptr;
  }

  if (m_connection.get_selection_owner_unchecked(m_atom).owner<xcb_window_t>() == m_tray) {
    m_log.trace("tray: Unset selection owner");
    m_connection.set_selection_owner(XCB_NONE, m_atom, XCB_CURRENT_TIME);
  }

  m_log.trace("tray: Unembed clients");
  m_clients.clear();

  if (m_tray != XCB_NONE) {
    if (m_mapped) {
      m_log.trace("tray: Unmap window");
      m_connection.unmap_window(m_tray);
      m_mapped = false;
    }

    m_log.trace("tray: Destroy window");
    m_connection.destroy_window(m_tray);
    m_tray = XCB_NONE;
    m_hidden = false;
  }

  m_connection.flush();
}  // }}}

/**
 * Reconfigure tray
 */
void traymanager::reconfigure() {  // {{{
  // Skip if tray window doesn't exist or if it's
  // in pseudo-hidden state
  if (m_tray == XCB_NONE || m_hidden) {
    return;
  }

  if (!m_mtx.try_lock()) {
    m_log.err("already locked");
    return;
  }

  std::lock_guard<std::mutex> lock(m_mtx, std::adopt_lock);

  reconfigure_clients();
  reconfigure_window();

  m_connection.flush();

  // Report status
  if (g_signals::tray::report_slotcount) {
    m_settings.slots = mapped_clients();
    g_signals::tray::report_slotcount(m_settings.slots);
  }
}  // }}}

/**
 * Reconfigure container window
 */
void traymanager::reconfigure_window() {  // {{{
  auto clients = mapped_clients();

  if (!clients && m_mapped) {
    m_connection.unmap_window(m_tray);
  } else if (clients && !m_mapped) {
    m_connection.map_window(m_tray);
  } else if (clients) {
    // clear window to get rid of frozen artifacts
    m_connection.clear_area(1, m_tray, 0, 0, 0, 0);

    // configure window
    uint32_t mask = 0;
    uint32_t values[7];
    xcb_params_configure_window_t params;

    XCB_AUX_ADD_PARAM(&mask, &params, width, calculate_w());
    XCB_AUX_ADD_PARAM(&mask, &params, x, calculate_x(params.width));

    xutils::pack_values(mask, &params, values);
    m_connection.configure_window_checked(m_tray, mask, values);
  }
}  // }}}

/**
 * Reconfigure clients
 */
void traymanager::reconfigure_clients() {  // {{{
  uint32_t x = m_settings.spacing;

  for (auto it = m_clients.rbegin(); it != m_clients.rend(); it++) {
    auto client = *it;

    try {
      client->ensure_state();
      client->reconfigure(x, calculate_client_y());

      x += m_settings.width + m_settings.spacing;
    } catch (const xpp::x::error::window& err) {
      remove_client(client, false);
    }
  }
}  // }}}

/**
 * Find the systray selection atom
 */
void traymanager::query_atom() {  // {{{
  m_log.trace("tray: Find systray selection atom for the default screen");
  string name{"_NET_SYSTEM_TRAY_S" + to_string(m_connection.default_screen())};
  auto reply = m_connection.intern_atom(false, name.length(), name.c_str());
  m_atom = reply.atom();
}  // }}}

/**
 * Create tray window
 */
void traymanager::create_window() {  // {{{
  auto scr = m_connection.screen();
  auto x = calculate_x(0);
  auto y = calculate_y();
  auto w = m_settings.width + m_settings.spacing * 2;
  auto h = m_settings.height_fill;

  m_tray = m_connection.generate_id();
  m_log.trace("tray: Create tray window %s, (%ix%i+%i+%i)", m_connection.id(m_tray), w, h, x, y);

  uint32_t mask = 0;
  uint32_t values[16];
  xcb_params_cw_t params;

  XCB_AUX_ADD_PARAM(&mask, &params, back_pixel, m_settings.background);
  XCB_AUX_ADD_PARAM(&mask, &params, border_pixel, m_settings.background);
  XCB_AUX_ADD_PARAM(&mask, &params, override_redirect, true);
  XCB_AUX_ADD_PARAM(&mask, &params, event_mask,
      XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_STRUCTURE_NOTIFY);

  xutils::pack_values(mask, &params, values);
  m_connection.create_window_checked(scr->root_depth, m_tray, scr->root, x, y, w, h, 0,
      XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual, mask, values);

  try {
    // Put the tray window above the defined sibling in the window stack
    if (m_settings.sibling != 0) {
      uint32_t configure_mask = 0;
      uint32_t configure_values[7];
      xcb_params_configure_window_t configure_params;

      XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, sibling, m_settings.sibling);
      XCB_AUX_ADD_PARAM(&configure_mask, &configure_params, stack_mode, XCB_STACK_MODE_ABOVE);

      xutils::pack_values(configure_mask, &configure_params, configure_values);
      m_connection.configure_window_checked(m_tray, configure_mask, configure_values);

      m_restacked = true;
    }
  } catch (const std::exception& err) {
    auto id = m_connection.id(m_settings.sibling);
    m_log.trace("tray: Failed to put tray above %s in the stack (%s)", id, err.what());
  }
}  // }}}

/**
 * Set window WM hints
 */
void traymanager::set_wmhints() {  // {{{
  m_log.trace("tray: Set window WM_NAME / WM_CLASS", m_connection.id(m_tray));
  xcb_icccm_set_wm_name(m_connection, m_tray, XCB_ATOM_STRING, 8, 22, TRAY_WM_NAME);
  xcb_icccm_set_wm_class(m_connection, m_tray, 15, TRAY_WM_CLASS);

  m_log.trace("tray: Set window WM_PROTOCOLS");
  vector<xcb_atom_t> wm_flags;
  wm_flags.emplace_back(WM_DELETE_WINDOW);
  wm_flags.emplace_back(WM_TAKE_FOCUS);
  xcb_icccm_set_wm_protocols(m_connection, m_tray, WM_PROTOCOLS, wm_flags.size(), wm_flags.data());

  m_log.trace("tray: Set window _NET_WM_WINDOW_TYPE");
  vector<xcb_atom_t> types;
  types.emplace_back(_NET_WM_WINDOW_TYPE_DOCK);
  types.emplace_back(_NET_WM_WINDOW_TYPE_NORMAL);
  m_connection.change_property(XCB_PROP_MODE_REPLACE, m_tray, _NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM,
      32, types.size(), types.data());

  m_log.trace("tray: Set window _NET_WM_STATE");
  vector<xcb_atom_t> states;
  states.emplace_back(_NET_WM_STATE_SKIP_TASKBAR);
  m_connection.change_property(XCB_PROP_MODE_REPLACE, m_tray, _NET_WM_STATE, XCB_ATOM_ATOM, 32,
      states.size(), states.data());

  m_log.trace("tray: Set window _NET_SYSTEM_TRAY_ORIENTATION");
  const uint32_t values[1]{_NET_SYSTEM_TRAY_ORIENTATION_HORZ};
  m_connection.change_property(XCB_PROP_MODE_REPLACE, m_tray, _NET_SYSTEM_TRAY_ORIENTATION,
      _NET_SYSTEM_TRAY_ORIENTATION, 32, 1, values);

  m_log.trace("tray: Set window _NET_SYSTEM_TRAY_VISUAL");
  const uint32_t values2[1]{m_connection.screen()->root_visual};
  m_connection.change_property(
      XCB_PROP_MODE_REPLACE, m_tray, _NET_SYSTEM_TRAY_VISUAL, XCB_ATOM_VISUALID, 32, 1, values2);

  m_log.trace("tray: Set window _NET_WM_PID");
  int pid = getpid();
  m_connection.change_property(
      XCB_PROP_MODE_REPLACE, m_tray, _NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);
}  // }}}

/**
 * Set color atom used by clients when determing icon theme
 */
void traymanager::set_traycolors() {  // {{{
  m_log.trace("tray: Set _NET_SYSTEM_TRAY_COLORS to %x", m_settings.background);

  auto r = color_util::red_channel(m_settings.background);
  auto g = color_util::green_channel(m_settings.background);
  auto b = color_util::blue_channel(m_settings.background);

  const uint32_t colors[12] = {
      r, g, b,  // normal
      r, g, b,  // error
      r, g, b,  // warning
      r, g, b,  // success
  };

  m_connection.change_property(
      XCB_PROP_MODE_REPLACE, m_tray, _NET_SYSTEM_TRAY_COLORS, XCB_ATOM_CARDINAL, 32, 12, colors);
}  // }}}

/**
 * Acquire the systray selection
 */
void traymanager::acquire_selection() {  // {{{
  xcb_window_t owner = m_connection.get_selection_owner_unchecked(m_atom)->owner;

  if (owner == m_tray) {
    m_log.info("tray: Already managing the systray selection");
    return;
  } else if ((m_othermanager = owner) != XCB_NONE) {
    m_log.info("Replacing selection manager %s", m_connection.id(owner));
  }

  m_log.trace("tray: Change selection owner to %s", m_connection.id(m_tray));
  m_connection.set_selection_owner_checked(m_tray, m_atom, XCB_CURRENT_TIME);

  if (m_connection.get_selection_owner_unchecked(m_atom)->owner != m_tray)
    throw application_error("Failed to get control of the systray selection");
}  // }}}

/**
 * Notify pending clients about the new systray MANAGER
 */
void traymanager::notify_clients() {  // {{{
  m_log.trace("tray: Broadcast new selection manager to pending clients");
  auto message = m_connection.make_client_message(MANAGER, m_connection.root());
  message->data.data32[0] = XCB_CURRENT_TIME;
  message->data.data32[1] = m_atom;
  message->data.data32[2] = m_tray;
  m_connection.send_client_message(message, m_connection.root());
}  // }}}

/**
 * Track changes to the given selection owner
 * If it gets destroyed or goes away we can reactivate the traymanager
 */
void traymanager::track_selection_owner(xcb_window_t owner) {  // {{{
  if (owner == XCB_NONE)
    return;
  m_log.trace("tray: Listen for events on the new selection window");
  const uint32_t value_list[1]{XCB_EVENT_MASK_STRUCTURE_NOTIFY};
  m_connection.change_window_attributes(owner, XCB_CW_EVENT_MASK, value_list);
}  // }}}

/**
 * Process client docking request
 */
void traymanager::process_docking_request(xcb_window_t win) {  // {{{
  auto client = find_client(win);

  if (client) {
    m_log.trace("tray: Client %s is already embedded, skipping...", m_connection.id(win));
    return;
  }

  m_log.trace("tray: Process docking request from %s", m_connection.id(win));
  m_clients.emplace_back(
      make_shared<trayclient>(m_connection, win, m_settings.width, m_settings.height));
  client = m_clients.back();

  try {
    m_log.trace("tray: Get client _XEMBED_INFO");
    xembed::query(m_connection, win, client->xembed());
  } catch (const application_error& err) {
    m_log.err(err.what());
  } catch (const xpp::x::error::window& err) {
    m_log.err("Failed to query for _XEMBED_INFO, removing client... (%s)", err.what());
    remove_client(client, false);
    return;
  }

  try {
    m_log.trace("tray: Update client window");
    {
      uint32_t mask = 0;
      uint32_t values[16];
      xcb_params_cw_t params;

      XCB_AUX_ADD_PARAM(&mask, &params, event_mask,
          XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY);

      if (m_settings.background != 0) {
        XCB_AUX_ADD_PARAM(&mask, &params, back_pixmap, XCB_BACK_PIXMAP_PARENT_RELATIVE);
      }

      xutils::pack_values(mask, &params, values);
      m_connection.change_window_attributes_checked(client->window(), mask, values);
    }

    m_log.trace("tray: Configure client size");
    client->reconfigure(0, 0);

    m_log.trace("tray: Add client window to the save set");
    m_connection.change_save_set_checked(XCB_SET_MODE_INSERT, client->window());

    m_log.trace("tray: Reparent client");
    m_connection.reparent_window_checked(
        client->window(), m_tray, calculate_client_x(client->window()), calculate_client_y());

    m_log.trace("tray: Send embbeded notification to client");
    xembed::notify_embedded(m_connection, client->window(), m_tray, client->xembed()->version);

    if (client->xembed()->flags & XEMBED_MAPPED) {
      m_log.trace("tray: Map client");
      m_connection.map_window_checked(client->window());
    }
  } catch (const xpp::x::error::window& err) {
    m_log.err("Failed to setup tray client, removing... (%s)", err.what());
    remove_client(client, false);
  }
}  // }}}

/**
 * Signal handler connected to the bar window's visibility change signal.
 * This is used as a fallback in case the window restacking fails. It will
 * toggle the tray window whenever the visibility of the bar window changes.
 */
void traymanager::bar_visibility_change(bool state) {  // {{{
  // Ignore unchanged states
  if (m_hidden == !state) {
    return;
  }

  // Update the psuedo-state
  m_hidden = !state;

  if (!m_hidden && !m_mapped) {
    m_connection.map_window(m_tray);
  } else if (m_hidden && m_mapped) {
    m_connection.unmap_window(m_tray);
  } else {
    return;
  }

  m_connection.flush();
}  // }}}

/**
 * Calculate x position of tray window
 */
int16_t traymanager::calculate_x(uint32_t width) const {  // {{{
  auto x = m_settings.orig_x;
  if (m_settings.align == alignment::RIGHT)
    x -= ((m_settings.width + m_settings.spacing) * m_clients.size() + m_settings.spacing);
  else if (m_settings.align == alignment::CENTER)
    x -= (width / 2) - (m_settings.width / 2);
  return x;
}  // }}}

/**
 * Calculate y position of tray window
 */
int16_t traymanager::calculate_y() const {  // {{{
  return m_settings.orig_y;
}  // }}}

/**
 * Calculate width of tray window
 */
uint16_t traymanager::calculate_w() const {  // {{{
  uint32_t width = m_settings.spacing;

  for (auto&& client : m_clients) {
    if (client->mapped()) {
      width += m_settings.spacing + m_settings.width;
    }
  }

  return width;
}  // }}}

/**
 * Calculate height of tray window
 */
uint16_t traymanager::calculate_h() const {  // {{{
  return m_settings.height_fill;
}  // }}}

/**
 * Calculate x position of client window
 */
int16_t traymanager::calculate_client_x(const xcb_window_t& win) {  // {{{
  for (size_t i = 0; i < m_clients.size(); i++)
    if (m_clients[i]->match(win))
      return m_settings.spacing + m_settings.width * i;
  return m_settings.spacing;
}  // }}}

/**
 * Calculate y position of client window
 */
int16_t traymanager::calculate_client_y() {  // {{{
  return (m_settings.height_fill - m_settings.height) / 2;
}  // }}}

/**
 * Find tray client by window
 */
shared_ptr<trayclient> traymanager::find_client(const xcb_window_t& win) const {  // {{{
  for (auto&& client : m_clients)
    if (client->match(win)) {
      return shared_ptr<trayclient>{client.get(), null_deleter{}};
    }
  return {};
}  // }}}

/**
 * Client error handling
 */
void traymanager::remove_client(shared_ptr<trayclient>& client, bool reconfigure) {  // {{{
  m_clients.erase(std::find(m_clients.begin(), m_clients.end(), client));

  if (reconfigure) {
    traymanager::reconfigure();
  }
}  // }}}

/**
 * Get number of mapped clients
 */
int traymanager::mapped_clients() const {  // {{{
  int mapped_clients = 0;

  for (auto&& client : m_clients) {
    if (client->mapped()) {
      mapped_clients++;
    }
  }

  return mapped_clients;
}  // }}}

/**
 * Event callback : XCB_EXPOSE
 */
void traymanager::handle(const evt::expose& evt) {  // {{{
  if (!m_activated || m_clients.empty())
    return;
  m_log.trace("tray: Received expose event for %s", m_connection.id(evt->window));
  reconfigure();
}  // }}}

/**
 * Event callback : XCB_VISIBILITY_NOTIFY
 */
void traymanager::handle(const evt::visibility_notify& evt) {  // {{{
  if (!m_activated || m_clients.empty())
    return;
  m_log.trace("tray: Received visibility_notify for %s", m_connection.id(evt->window));
  reconfigure();
}  // }}}

/**
 * Event callback : XCB_CLIENT_MESSAGE
 */
void traymanager::handle(const evt::client_message& evt) {  // {{{
  if (!m_activated) {
    return;
  }

  if (evt->type == _NET_SYSTEM_TRAY_OPCODE && evt->format == 32) {
    m_log.trace("tray: Received client_message");

    switch (evt->data.data32[1]) {
      case SYSTEM_TRAY_REQUEST_DOCK:
        try {
          process_docking_request(evt->data.data32[2]);
        } catch (const std::exception& err) {
          auto id = m_connection.id(evt->data.data32[2]);
          m_log.err("Error while processing docking request for %s (%s)", id, err.what());
        }
        return;

      case SYSTEM_TRAY_BEGIN_MESSAGE:
        // process_messages(...);
        return;

      case SYSTEM_TRAY_CANCEL_MESSAGE:
        // process_messages(...);
        return;
    }
  } else if (evt->type == WM_PROTOCOLS && evt->data.data32[0] == WM_DELETE_WINDOW) {
    if (evt->window == m_tray) {
      m_log.warn("Received WM_DELETE");
      m_tray = XCB_NONE;
      deactivate();
    }
  }
}  // }}}

/**
 * Event callback : XCB_CONFIGURE_REQUEST
 *
 * Called when a tray client thinks he's part of the free world and
 * wants to reconfigure its window. This is of course nothing we appreciate
 * so we return an answer that'll put him in place.
 */
void traymanager::handle(const evt::configure_request& evt) {  // {{{
  if (!m_activated)
    return;

  auto client = find_client(evt->window);

  if (!client)
    return;

  try {
    m_log.trace("tray: Client configure request %s", m_connection.id(evt->window));
    client->configure_notify(calculate_client_x(evt->window), calculate_client_y());
  } catch (const xpp::x::error::window& err) {
    m_log.err("Failed to reconfigure tray client, removing... (%s)", err.what());
    remove_client(client);
  }
}  // }}}

/**
 * @see tray_manager::handle(const evt::configure_request&);
 */
void traymanager::handle(const evt::resize_request& evt) {  // {{{
  if (!m_activated)
    return;

  auto client = find_client(evt->window);

  if (!client)
    return;

  try {
    m_log.trace("tray: Received resize_request for client %s", m_connection.id(evt->window));
    client->configure_notify(calculate_client_x(evt->window), calculate_client_y());
  } catch (const xpp::x::error::window& err) {
    m_log.err("Failed to reconfigure tray client, removing... (%s)", err.what());
    remove_client(client);
  }
}  // }}}

/**
 * Event callback : XCB_SELECTION_CLEAR
 */
void traymanager::handle(const evt::selection_clear& evt) {  // {{{
  if (!m_activated)
    return;
  if (evt->selection != m_atom)
    return;
  if (evt->owner != m_tray)
    return;

  try {
    m_log.warn("Lost systray selection, deactivating...");
    m_othermanager = m_connection.get_selection_owner(m_atom)->owner;
    track_selection_owner(m_othermanager);
  } catch (const std::exception& err) {
    m_log.err("Failed to get systray selection owner");
    m_othermanager = XCB_NONE;
  }

  deactivate();
}  // }}}

/**
 * Event callback : XCB_PROPERTY_NOTIFY
 */
void traymanager::handle(const evt::property_notify& evt) {  // {{{
  if (!m_activated)
    return;
  if (evt->atom != _XEMBED_INFO)
    return;

  m_log.trace("tray: _XEMBED_INFO: %s", m_connection.id(evt->window));

  auto client = find_client(evt->window);
  if (!client)
    return;

  auto xd = client->xembed();
  auto win = client->window();

  if (evt->state == XCB_PROPERTY_NEW_VALUE) {
    m_log.trace("tray: _XEMBED_INFO value has changed");
  }

  xembed::query(m_connection, win, xd);
  m_log.trace("tray: _XEMBED_INFO[0]=%u _XEMBED_INFO[1]=%u", xd->version, xd->flags);

  reconfigure();
}  // }}}

/**
 * Event callback : XCB_REPARENT_NOTIFY
 */
void traymanager::handle(const evt::reparent_notify& evt) {  // {{{
  if (!m_activated)
    return;

  auto client = find_client(evt->window);

  if (client && evt->parent != m_tray) {
    m_log.trace("tray: Received reparent_notify for client, remove...");
    remove_client(client);
  }
}  // }}}

/**
 * Event callback : XCB_DESTROY_NOTIFY
 */
void traymanager::handle(const evt::destroy_notify& evt) {  // {{{
  if (!m_activated && evt->window == m_othermanager) {
    m_log.trace("tray: Received destroy_notify");
    m_log.trace("tray: Systray selection is available... re-activating");
    activate();
  } else if (m_activated) {
    auto client = find_client(evt->window);
    if (client) {
      m_log.trace("tray: Received destroy_notify for client, remove...");
      remove_client(client);
    }
  }
}  // }}}

/**
 * Event callback : XCB_MAP_NOTIFY
 */
void traymanager::handle(const evt::map_notify& evt) {  // {{{
  if (!m_activated)
    return;

  if (evt->window == m_tray && !m_mapped) {
    if (m_mapped)
      return;
    m_log.trace("tray: Received map_notify");
    m_log.trace("tray: Update container mapped flag");
    m_mapped = true;
    reconfigure();
  } else {
    auto client = find_client(evt->window);
    if (client) {
      m_log.trace("tray: Received map_notify");
      m_log.trace("tray: Set client mapped");
      client->mapped(true);
      reconfigure();
    }
  }
}  // }}}

/**
 * Event callback : XCB_UNMAP_NOTIFY
 */
void traymanager::handle(const evt::unmap_notify& evt) {  // {{{
  if (!m_activated)
    return;

  if (evt->window == m_tray) {
    m_log.trace("tray: Received unmap_notify");
    if (!m_mapped)
      return;
    m_log.trace("tray: Update container mapped flag");
    m_mapped = false;
    reconfigure();
  } else {
    auto client = find_client(evt->window);
    if (client) {
      m_log.trace("tray: Received unmap_notify");
      m_log.trace("tray: Set client unmapped");
      client->mapped(true);
      reconfigure();
    }
  }
}  // }}}

// }}}

LEMONBUDDY_NS_END
