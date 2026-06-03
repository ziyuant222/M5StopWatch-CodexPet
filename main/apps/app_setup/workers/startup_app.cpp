/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "workers.h"
#include "../view/view.h"
#include <hal/hal.h>
#include <hal/utils/settings/settings.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace setup_workers;
using namespace mooncake;

namespace {

static const std::string _tag = "Setup-StartupApp";
constexpr const char* STARTUP_APP_KEY = "startup_app";
constexpr const char* STARTUP_APP_DEFAULT = "CodexBuddy";
constexpr const char* STARTUP_APP_LAUNCHER = "Launcher";
constexpr const char* SYSTEM_SETTINGS_NS = "system";

}  // namespace

class StartupAppWorker::StartupAppView {
public:
    StartupAppView()
    {
        Settings settings(SYSTEM_SETTINGS_NS, false);
        _current = settings.GetString(STARTUP_APP_KEY, STARTUP_APP_DEFAULT);

        std::vector<view::SelectMenuPage::MenuItem> items;
        std::set<std::string> seen;

        add_item(items, seen, STARTUP_APP_LAUNCHER);
        for (const auto& props : GetMooncake().getAllAppProps()) {
            if (!props.info.name.empty()) {
                add_item(items, seen, props.info.name);
            }
        }

        _menu = std::make_unique<view::SelectMenuPage>(
            std::vector<view::SelectMenuPage::MenuSection>{{"Startup App", std::move(items)}});
    }

    void update()
    {
        if (_menu) {
            _menu->update();
        }
    }

    bool consumeDone()
    {
        const bool done = _done;
        _done = false;
        return done;
    }

private:
    std::unique_ptr<view::SelectMenuPage> _menu;
    std::string _current;
    bool _done = false;

    void add_item(
        std::vector<view::SelectMenuPage::MenuItem>& items,
        std::set<std::string>& seen,
        const std::string& name)
    {
        if (seen.count(name) > 0) {
            return;
        }
        seen.insert(name);

        const std::string label = name == _current ? "* " + name : name;
        items.push_back({label, [this, name]() {
                             Settings settings(SYSTEM_SETTINGS_NS, true);
                             settings.SetString(STARTUP_APP_KEY, name);
                             mclog::tagInfo(_tag, "startup app set to {}", name);
                             _done = true;
                         }});
    }
};

StartupAppWorker::StartupAppWorker()
{
    mclog::tagInfo(_tag, "create");
    _view = std::make_unique<StartupAppView>();
}

StartupAppWorker::~StartupAppWorker()
{
    mclog::tagInfo(_tag, "destroy");
}

void StartupAppWorker::update()
{
    if (_view) {
        _view->update();
        if (_view->consumeDone()) {
            _is_done = true;
        }
    }
}
