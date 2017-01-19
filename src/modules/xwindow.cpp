#include "modules/xwindow.hpp"
#include "drawtypes/label.hpp"
#include "utils/factory.hpp"
#include "x11/atoms.hpp"
#include "x11/connection.hpp"
#include "x11/graphics.hpp"

#include "modules/meta/base.inl"

POLYBAR_NS

namespace modules {
  template class module<xwindow_module>;

  /**
   * Wrapper used to update the event mask of the
   * currently active to enable title tracking
   */
  active_window::active_window(xcb_connection_t* conn, xcb_window_t win) : m_connection(conn), m_window(win) {
    if (m_window != XCB_NONE) {
      const unsigned int mask{XCB_EVENT_MASK_PROPERTY_CHANGE};
      xcb_change_window_attributes(m_connection, m_window, XCB_CW_EVENT_MASK, &mask);
    }
  }

  /**
   * Deconstruct window object
   */
  active_window::~active_window() {
    if (m_window != XCB_NONE) {
      const unsigned int mask{XCB_EVENT_MASK_NO_EVENT};
      xcb_change_window_attributes(m_connection, m_window, XCB_CW_EVENT_MASK, &mask);
    }
  }

  /**
   * Check if current window matches passed value
   */
  bool active_window::match(const xcb_window_t win) const {
    return m_window == win;
  }

  /**
   * Get the title by returning the first non-empty value of:
   *  _NET_WM_NAME
   *  _NET_WM_VISIBLE_NAME
   */
  string active_window::title(xcb_ewmh_connection_t* ewmh) const {
    string title;

    if (!(title = ewmh_util::get_wm_name(ewmh, m_window)).empty()) {
      return title;
    } else if (!(title = ewmh_util::get_visible_name(ewmh, m_window)).empty()) {
      return title;
    } else if (!(title = icccm_util::get_wm_name(m_connection, m_window)).empty()) {
      return title;
    } else {
      return "";
    }
  }

  /**
   * Construct module
   */
  xwindow_module::xwindow_module(const bar_settings& bar, string name_)
      : static_module<xwindow_module>(bar, move(name_)), m_connection(connection::make()) {
    // Initialize ewmh atoms
    if ((m_ewmh = ewmh_util::initialize()) == nullptr) {
      throw module_error("Failed to initialize ewmh atoms");
    }

    // Check if the WM supports _NET_ACTIVE_WINDOW
    if (!ewmh_util::supports(m_ewmh.get(), _NET_ACTIVE_WINDOW)) {
      throw module_error("The WM does not list _NET_ACTIVE_WINDOW as a supported hint");
    }

    // Add formats and elements
    m_formatter->add(DEFAULT_FORMAT, TAG_LABEL, {TAG_LABEL});

    if (m_formatter->has(TAG_LABEL)) {
      m_label = load_optional_label(m_conf, name(), TAG_LABEL, "%title%");
    }
  }

  /**
   * Handler for XCB_PROPERTY_NOTIFY events
   */
  void xwindow_module::handle(const evt::property_notify& evt) {
    if (evt->atom == _NET_ACTIVE_WINDOW) {
      update(true);
    } else if (evt->atom == _NET_CURRENT_DESKTOP) {
      update(true);
    } else if (evt->atom == _NET_WM_VISIBLE_NAME) {
      update();
    } else if (evt->atom == _NET_WM_NAME) {
      update();
    } else {
      return;
    }
  }

  /**
   * Update the currently active window and query its title
   */
  void xwindow_module::update(bool force) {
    xcb_window_t win;

    if (force) {
      m_active.reset();
    }

    if (!m_active && (win = ewmh_util::get_active_window(&*m_ewmh)) != XCB_NONE) {
      m_active = make_unique<active_window>(m_connection, win);
    }

    if (m_label) {
      m_label->reset_tokens();
      m_label->replace_token("%title%", m_active ? m_active->title(&*m_ewmh) : "");
    }

    broadcast();
  }

  /**
   * Output content as defined in the config
   */
  bool xwindow_module::build(builder* builder, const string& tag) const {
    if (tag == TAG_LABEL && m_label && m_label.get()) {
      builder->node(m_label);
      return true;
    }
    return false;
  }
}

POLYBAR_NS_END
