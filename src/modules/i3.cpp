#include <sys/socket.h>

#include "drawtypes/iconset.hpp"
#include "drawtypes/label.hpp"
#include "modules/i3.hpp"
#include "utils/factory.hpp"
#include "utils/file.hpp"

#include "modules/meta/base.inl"
#include "modules/meta/event_module.inl"

POLYBAR_NS

namespace modules {
  template class module<i3_module>;
  template class event_module<i3_module>;

  i3_module::workspace::operator bool() {
    return label && *label;
  }

  void i3_module::setup() {
    auto socket_path = i3ipc::get_socketpath();

    if (!file_util::exists(socket_path)) {
      throw module_error("Could not find socket: " + (socket_path.empty() ? "<empty>" : socket_path));
    }

    m_ipc = factory_util::unique<i3ipc::connection>();

    // Load configuration values
    GET_CONFIG_VALUE(name(), m_click, "enable-click");
    GET_CONFIG_VALUE(name(), m_scroll, "enable-scroll");
    GET_CONFIG_VALUE(name(), m_revscroll, "reverse-scroll");
    GET_CONFIG_VALUE(name(), m_wrap, "wrapping-scroll");
    GET_CONFIG_VALUE(name(), m_indexsort, "index-sort");
    GET_CONFIG_VALUE(name(), m_pinworkspaces, "pin-workspaces");
    GET_CONFIG_VALUE(name(), m_strip_wsnumbers, "strip-wsnumbers");

    m_conf.warn_deprecated(name(), "wsname-maxlen", "%name:min:max%");

    // Add formats and create components
    m_formatter->add(DEFAULT_FORMAT, DEFAULT_TAGS, {TAG_LABEL_STATE, TAG_LABEL_MODE});

    if (m_formatter->has(TAG_LABEL_STATE)) {
      m_statelabels.insert(
          make_pair(state::FOCUSED, load_optional_label(m_conf, name(), "label-focused", DEFAULT_WS_LABEL)));
      m_statelabels.insert(
          make_pair(state::UNFOCUSED, load_optional_label(m_conf, name(), "label-unfocused", DEFAULT_WS_LABEL)));
      m_statelabels.insert(
          make_pair(state::VISIBLE, load_optional_label(m_conf, name(), "label-visible", DEFAULT_WS_LABEL)));
      m_statelabels.insert(
          make_pair(state::URGENT, load_optional_label(m_conf, name(), "label-urgent", DEFAULT_WS_LABEL)));
    }

    if (m_formatter->has(TAG_LABEL_MODE)) {
      m_modelabel = load_optional_label(m_conf, name(), "label-mode", "%mode%");
    }

    m_icons = factory_util::shared<iconset>();
    m_icons->add(DEFAULT_WS_ICON, factory_util::shared<label>(m_conf.get<string>(name(), DEFAULT_WS_ICON, "")));

    for (const auto& workspace : m_conf.get_list<string>(name(), "ws-icon", {})) {
      auto vec = string_util::split(workspace, ';');
      if (vec.size() == 2) {
        m_icons->add(vec[0], factory_util::shared<label>(vec[1]));
      }
    }

    try {
      if (m_modelabel) {
        m_ipc->on_mode_event = [this](const i3ipc::mode_t& mode) {
          m_modeactive = (mode.change != DEFAULT_MODE);
          if (m_modeactive) {
            m_modelabel->reset_tokens();
            m_modelabel->replace_token("%mode%", mode.change);
          }
        };
      }
      m_ipc->subscribe(i3ipc::ET_WORKSPACE | i3ipc::ET_MODE);
    } catch (const exception& err) {
      throw module_error(err.what());
    }
  }

  void i3_module::stop() {
    try {
      if (m_ipc) {
        m_log.info("%s: Disconnecting from socket", name());
        shutdown(m_ipc->get_event_socket_fd(), SHUT_RDWR);
        shutdown(m_ipc->get_main_socket_fd(), SHUT_RDWR);
      }
    } catch (...) {
    }

    event_module::stop();
  }

  bool i3_module::has_event() {
    try {
      m_ipc->handle_event();
      return true;
    } catch (const exception& err) {
      return false;
    }
  }

  bool i3_module::update() {
    m_workspaces.clear();
    i3_util::connection_t ipc;

    try {
      vector<shared_ptr<i3_util::workspace_t>> workspaces;

      if (m_pinworkspaces) {
        workspaces = i3_util::workspaces(ipc, m_bar.monitor->name);
      } else {
        workspaces = i3_util::workspaces(ipc);
      }

      if (m_indexsort) {
        sort(workspaces.begin(), workspaces.end(), i3_util::ws_numsort);
      }

      for (auto&& ws : workspaces) {
        state ws_state{state::NONE};

        if (ws->focused) {
          ws_state = state::FOCUSED;
        } else if (ws->urgent) {
          ws_state = state::URGENT;
        } else if (!ws->visible || (ws->visible && ws->output != m_bar.monitor->name)) {
          ws_state = state::UNFOCUSED;
        } else {
          ws_state = state::VISIBLE;
        }

        string ws_name{ws->name};

        // Remove workspace numbers "0:"
        if (m_strip_wsnumbers) {
          ws_name.erase(0, string_util::find_nth(ws_name, 0, ":", 1) + 1);
        }

        // Trim leading and trailing whitespace
        ws_name = string_util::trim(move(ws_name), ' ');

        auto icon = m_icons->get(ws->name, DEFAULT_WS_ICON);
        auto label = m_statelabels.find(ws_state)->second->clone();

        label->reset_tokens();
        label->replace_token("%output%", ws->output);
        label->replace_token("%name%", ws_name);
        label->replace_token("%icon%", icon->get());
        label->replace_token("%index%", to_string(ws->num));
        m_workspaces.emplace_back(factory_util::unique<workspace>(ws->num, ws_state, move(label)));
      }

      return true;
    } catch (const exception& err) {
      m_log.err("%s: %s", name(), err.what());
      return false;
    }
  }

  bool i3_module::build(builder* builder, const string& tag) const {
    if (tag == TAG_LABEL_MODE && m_modeactive) {
      builder->node(m_modelabel);
    } else if (tag == TAG_LABEL_STATE && !m_workspaces.empty()) {
      if (m_scroll) {
        builder->cmd(mousebtn::SCROLL_DOWN, EVENT_SCROLL_DOWN);
        builder->cmd(mousebtn::SCROLL_UP, EVENT_SCROLL_UP);
      }

      for (auto&& ws : m_workspaces) {
        if (m_click) {
          builder->cmd(mousebtn::LEFT, string{EVENT_CLICK} + to_string(ws->index));
          builder->node(ws->label);
          builder->cmd_close();
        } else {
          builder->node(ws->label);
        }
      }

      if (m_scroll) {
        builder->cmd_close();
        builder->cmd_close();
      }
    } else {
      return false;
    }

    return true;
  }

  bool i3_module::handle_event(string cmd) {
    if (cmd.find(EVENT_PREFIX) != 0) {
      return false;
    }

    try {
      string scrolldir;
      const i3_util::connection_t conn{};

      if (cmd.compare(0, strlen(EVENT_CLICK), EVENT_CLICK) == 0) {
        const string workspace_num{cmd.substr(strlen(EVENT_CLICK))};

        if (i3_util::focused_workspace(conn)->num != atoi(workspace_num.c_str())) {
          m_log.info("%s: Sending workspace focus command to ipc handler", name());
          conn.send_command("workspace number " + workspace_num);
        }
      } else if (cmd.compare(0, strlen(EVENT_SCROLL_DOWN), EVENT_SCROLL_DOWN) == 0) {
        scrolldir = m_revscroll ? "next" : "prev";
      } else if (cmd.compare(0, strlen(EVENT_SCROLL_UP), EVENT_SCROLL_UP) == 0) {
        scrolldir = m_revscroll ? "prev" : "next";
      } else {
        return false;
      }

      if (scrolldir == "next" && (m_wrap || *i3_util::workspaces(conn, m_bar.monitor->name).back() != *i3_util::focused_workspace(conn))) {
        m_log.info("%s: Sending workspace next command to ipc handler", name());
        i3_util::connection_t{}.send_command("workspace next_on_output");
      } else if (scrolldir == "prev" && (m_wrap || *i3_util::workspaces(conn, m_bar.monitor->name).front() != *i3_util::focused_workspace(conn))) {
        m_log.info("%s: Sending workspace prev command to ipc handler", name());
        i3_util::connection_t{}.send_command("workspace prev_on_output");
      }
    } catch (const exception& err) {
      m_log.err("%s: %s", name(), err.what());
    }

    return true;
  }
}

POLYBAR_NS_END
