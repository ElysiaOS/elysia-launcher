#include "ely_launcher.h"

class AppsManager {
private:
    std::vector<AppInfo> all_apps;
    std::vector<AppInfo> filtered_apps;
    std::vector<GtkWidget*> app_buttons;
    std::unordered_map<std::string, GdkPixbuf*> icon_cache;
    GdkPixbuf* placeholder_app_icon = nullptr;
    size_t current_page = 0;
    static constexpr size_t APPS_PER_PAGE = 7;
    int selected_index = -1; // index into filtered_apps
    EnhancedEdgeLauncher* launcher;

public:
    AppsManager(EnhancedEdgeLauncher* l) : launcher(l) {}

    ~AppsManager() {
        for (auto* button : app_buttons) {
            gtk_widget_destroy(button);
        }
        if (placeholder_app_icon) {
            g_object_unref(placeholder_app_icon);
        }
        for (auto& kv : icon_cache) {
            if (kv.second) g_object_unref(kv.second);
        }
    }

    void load_applications() {
        auto build_dirs = []() {
            std::unordered_set<std::string> uniq;
            auto add = [&](const std::string& p){ if (!p.empty()) uniq.insert(p); };
            add("/usr/share/applications");
            add("/usr/local/share/applications");
            add(std::string(g_get_home_dir()) + "/.local/share/applications");
            // Flatpak export dirs
            add(std::string(g_get_home_dir()) + "/.local/share/flatpak/exports/share/applications");
            add("/var/lib/flatpak/exports/share/applications");
            // XDG_DATA_DIRS
            const char* xdg = g_getenv("XDG_DATA_DIRS");
            if (xdg && *xdg) {
                std::string dirs = xdg;
                size_t start = 0;
                while (start <= dirs.size()) {
                    size_t pos = dirs.find(":", start);
                    std::string base = (pos == std::string::npos) ? dirs.substr(start) : dirs.substr(start, pos - start);
                    if (!base.empty()) add(base + "/applications");
                    if (pos == std::string::npos) break;
                    start = pos + 1;
                }
            }
            return std::vector<std::string>(uniq.begin(), uniq.end());
        }();

        std::unordered_set<std::string> seen_ids; // desktop file id (filename)
        all_apps.clear();
        all_apps.reserve(200);

        for (const auto& dir : build_dirs) {
            DIR* directory = opendir(dir.c_str());
            if (!directory) continue;
            struct dirent* entry;
            while ((entry = readdir(directory)) != nullptr) {
                std::string filename = entry->d_name;
                if (filename.length() <= 8 || filename.substr(filename.length() - 8) != ".desktop") {
                    continue;
                }
                std::string filepath = dir + "/" + filename;
                bool exclude = false;
                AppInfo app = parse_desktop_file(filepath, exclude);
                if (!exclude && !app.name.empty() && !app.exec.empty()) {
                    // de-duplicate by desktop id (filename)
                    if (seen_ids.find(filename) != seen_ids.end()) continue;
                    auto cache_it = launcher->get_app_usage_cache().find(app.name);
                    if (cache_it != launcher->get_app_usage_cache().end()) {
                        app.usage_count = cache_it->second;
                    }
                    seen_ids.insert(filename);
                    all_apps.push_back(app);
                }
            }
            closedir(directory);
        }
        std::sort(all_apps.begin(), all_apps.end(),
                  [](const AppInfo& a, const AppInfo& b) {
                      if (a.usage_count != b.usage_count) {
                          return a.usage_count > b.usage_count;
                      }
                      return a.name < b.name;
                  });
        filtered_apps = all_apps;
    }

    struct AppsLoadedPayload {
        AppsManager* manager;
        std::vector<AppInfo>* apps;
    };

    static gboolean on_apps_loaded_idle(gpointer data) {
        AppsLoadedPayload* payload = static_cast<AppsLoadedPayload*>(data);
        AppsManager* manager = payload->manager;
        if (manager && manager->launcher && manager->launcher->get_window() && GTK_IS_WIDGET(manager->launcher->get_window())) {
            manager->all_apps = std::move(*payload->apps);
            manager->filtered_apps = manager->all_apps;
            manager->filter_apps("");
        }
        delete payload->apps;
        delete payload;
        return G_SOURCE_REMOVE;
    }

    void load_applications_async() {
        std::thread([this]() {
            auto* apps = new std::vector<AppInfo>();
            apps->reserve(200);
            auto build_dirs = []() {
                std::unordered_set<std::string> uniq;
                auto add = [&](const std::string& p){ if (!p.empty()) uniq.insert(p); };
                add("/usr/share/applications");
                add("/usr/local/share/applications");
                add(std::string(g_get_home_dir()) + "/.local/share/applications");
                add(std::string(g_get_home_dir()) + "/.local/share/flatpak/exports/share/applications");
                add("/var/lib/flatpak/exports/share/applications");
                const char* xdg = g_getenv("XDG_DATA_DIRS");
                if (xdg && *xdg) {
                    std::string dirs = xdg;
                    size_t start = 0;
                    while (start <= dirs.size()) {
                        size_t pos = dirs.find(":", start);
                        std::string base = (pos == std::string::npos) ? dirs.substr(start) : dirs.substr(start, pos - start);
                        if (!base.empty()) add(base + "/applications");
                        if (pos == std::string::npos) break;
                        start = pos + 1;
                    }
                }
                return std::vector<std::string>(uniq.begin(), uniq.end());
            }();
            std::unordered_set<std::string> seen_ids;
            for (const auto& dir : build_dirs) {
                DIR* directory = opendir(dir.c_str());
                if (!directory) continue;
                struct dirent* entry;
                while ((entry = readdir(directory)) != nullptr) {
                    std::string filename = entry->d_name;
                    if (filename.length() <= 8 || filename.substr(filename.length() - 8) != ".desktop") {
                        continue;
                    }
                    std::string filepath = dir + "/" + filename;
                    bool exclude = false;
                    AppInfo app = parse_desktop_file(filepath, exclude);
                    if (!exclude && !app.name.empty() && !app.exec.empty()) {
                        if (seen_ids.find(filename) != seen_ids.end()) continue;
                        auto cache_it = launcher->get_app_usage_cache().find(app.name);
                        if (cache_it != launcher->get_app_usage_cache().end()) {
                            app.usage_count = cache_it->second;
                        }
                        seen_ids.insert(filename);
                        apps->push_back(app);
                    }
                }
                closedir(directory);
            }
            std::sort(apps->begin(), apps->end(), [](const AppInfo& a, const AppInfo& b) {
                if (a.usage_count != b.usage_count) {
                    return a.usage_count > b.usage_count;
                }
                return a.name < b.name;
            });
            auto* payload = new AppsLoadedPayload{this, apps};
            g_idle_add(on_apps_loaded_idle, payload);
        }).detach();
    }

    AppInfo parse_desktop_file(const std::string& filepath, bool& is_hidden_or_invalid) {
        AppInfo app;
        is_hidden_or_invalid = false;
        std::ifstream file(filepath);
        if (!file.is_open()) {
            is_hidden_or_invalid = true;
            return app;
        }
        std::string line;
        bool found_name = false, found_icon = false, found_exec = false;
        bool no_display = false, hidden = false, is_application_type = false;
        while (std::getline(file, line)) {
            if (!found_name && line.compare(0, 5, "Name=") == 0) {
                app.name = line.substr(5);
                found_name = true;
            } else if (!found_icon && line.compare(0, 5, "Icon=") == 0) {
                app.icon = line.substr(5);
                found_icon = true;
            } else if (!found_exec && line.compare(0, 5, "Exec=") == 0) {
                app.exec = line.substr(5);
                size_t pos = app.exec.find(" %");
                if (pos != std::string::npos) {
                    app.exec.erase(pos);
                }
                found_exec = true;
            } else if (line.compare(0, 10, "NoDisplay=") == 0) {
                std::string val = line.substr(10);
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                no_display = (val.find("true") != std::string::npos);
            } else if (line.compare(0, 7, "Hidden=") == 0) {
                std::string val = line.substr(7);
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                hidden = (val.find("true") != std::string::npos);
            } else if (line.compare(0, 5, "Type=") == 0) {
                std::string val = line.substr(5);
                // Require explicit Application type
                is_application_type = (val.find("Application") != std::string::npos);
            }
        }
        app.desktop_file = filepath;
        if (hidden || no_display || !is_application_type) {
            is_hidden_or_invalid = true;
        }
        return app;
    }

    static void on_app_clicked(GtkWidget* button, gpointer data) {
        AppInfo* app = static_cast<AppInfo*>(data);
        
        // Get launcher instance from button's user data
        EnhancedEdgeLauncher* launcher = static_cast<EnhancedEdgeLauncher*>(
            g_object_get_data(G_OBJECT(button), "launcher"));
        
        if (launcher) {
            launcher->increment_app_usage(app->name);
        }
        
        std::string command = app->exec + " &";
        system(command.c_str());
        gtk_main_quit();
    }

    GdkPixbuf* get_icon_pixbuf_cached(const std::string& icon_name_or_path, int icon_size) {
        std::string key = icon_name_or_path + "|" + std::to_string(icon_size);
        auto it = icon_cache.find(key);
        if (it != icon_cache.end() && it->second) {
            return GDK_PIXBUF(g_object_ref(it->second));
        }
        GdkPixbuf* pix = nullptr;
        GtkIconTheme* icon_theme = gtk_icon_theme_get_default();
        if (!icon_name_or_path.empty()) {
            if (g_file_test(icon_name_or_path.c_str(), G_FILE_TEST_EXISTS)) {
                pix = gdk_pixbuf_new_from_file_at_scale(icon_name_or_path.c_str(), icon_size, icon_size, FALSE, nullptr);
            } else {
                GdkPixbuf* loaded = gtk_icon_theme_load_icon(icon_theme, icon_name_or_path.c_str(), icon_size, GTK_ICON_LOOKUP_USE_BUILTIN, nullptr);
                if (loaded) {
                    pix = gdk_pixbuf_scale_simple(loaded, icon_size, icon_size, GDK_INTERP_BILINEAR);
                    g_object_unref(loaded);
                }
            }
        }
        if (!pix) {
            GdkPixbuf* loaded = gtk_icon_theme_load_icon(icon_theme, "application-x-executable", icon_size, GTK_ICON_LOOKUP_USE_BUILTIN, nullptr);
            if (loaded) {
                pix = gdk_pixbuf_scale_simple(loaded, icon_size, icon_size, GDK_INTERP_BILINEAR);
                g_object_unref(loaded);
            }
        }
        if (pix) {
            icon_cache[key] = GDK_PIXBUF(g_object_ref(pix));
            return pix;
        }
        return nullptr;
    }

    void create_app_buttons() {
        // Clear existing buttons
        for (auto* button : app_buttons) {
            gtk_widget_destroy(button);
        }
        app_buttons.clear();

        size_t start_idx = current_page * APPS_PER_PAGE;
        size_t end_idx = std::min(start_idx + APPS_PER_PAGE, filtered_apps.size());

        int start_y = 150;
        int button_spacing = 60;
        int icon_size = 50;
        int button_x = 85; // shift right to make room for mode buttons on the left

        // Ensure we have a placeholder icon ready
        if (!placeholder_app_icon) {
            GtkIconTheme* icon_theme = gtk_icon_theme_get_default();
            GdkPixbuf* loaded = gtk_icon_theme_load_icon(icon_theme, "application-x-executable", icon_size, GTK_ICON_LOOKUP_USE_BUILTIN, nullptr);
            if (loaded) {
                placeholder_app_icon = loaded;
            }
        }

        for (size_t i = start_idx; i < end_idx; i++) {
            GtkWidget* button = gtk_button_new();
            gtk_widget_set_name(button, "app-button");
            gtk_widget_set_size_request(button, icon_size + 8, icon_size + 8);

            // Show placeholder immediately
            if (placeholder_app_icon) {
                GtkWidget* icon = gtk_image_new_from_pixbuf(placeholder_app_icon);
                gtk_widget_set_size_request(icon, icon_size, icon_size);
                gtk_button_set_image(GTK_BUTTON(button), icon);
            }

            gtk_widget_set_tooltip_text(button, filtered_apps[i].name.c_str());
            g_signal_connect(button, "clicked", G_CALLBACK(on_app_clicked), &filtered_apps[i]);
            
            // Store launcher reference in button for callback access
            g_object_set_data(G_OBJECT(button), "launcher", launcher);
            g_object_set_data_full(G_OBJECT(button), "app_name", g_strdup(filtered_apps[i].name.c_str()), g_free);

            int y_pos = start_y + (i - start_idx) * button_spacing;
            gtk_fixed_put(GTK_FIXED(launcher->get_layout()), button, button_x, y_pos);

            gtk_widget_set_visible(button, TRUE);
            app_buttons.push_back(button);
        }
        update_selection_visuals();

        // Asynchronously load actual icons for visible buttons
        std::vector<std::pair<std::string,int>> to_load;
        to_load.reserve(end_idx - start_idx);
        for (size_t i = start_idx; i < end_idx; ++i) {
            to_load.emplace_back(filtered_apps[i].icon, icon_size);
        }
        std::thread([this, to_load, start_idx]() {
            for (size_t i = 0; i < to_load.size(); ++i) {
                const std::string& icon_id = to_load[i].first;
                int size = to_load[i].second;
                GdkPixbuf* pix = get_icon_pixbuf_cached(icon_id, size);
                if (!pix) continue;
                // Prepare payload
                struct Payload { AppsManager* self; GdkPixbuf* pix; std::string app_name; };
                size_t idx = start_idx + i;
                if (idx >= filtered_apps.size()) { g_object_unref(pix); continue; }
                auto* payload = new Payload{this, pix, filtered_apps[idx].name};
                g_idle_add(+[](gpointer data) -> gboolean {
                    auto* p = static_cast<Payload*>(data);
                    // Find matching button by app_name
                    for (GtkWidget* btn : p->self->app_buttons) {
                        const char* n = static_cast<const char*>(g_object_get_data(G_OBJECT(btn), "app_name"));
                        if (n && p->app_name == n) {
                            GtkWidget* img = gtk_image_new_from_pixbuf(p->pix);
                            gtk_button_set_image(GTK_BUTTON(btn), img);
                            break;
                        }
                    }
                    g_object_unref(p->pix);
                    delete p;
                    return G_SOURCE_REMOVE;
                }, payload);
            }
        }).detach();
    }

    void filter_apps(const std::string& query) {
        // Normalize query lowercase
        std::string ql = query;
        std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);

        if (ql.empty()) {
            filtered_apps = all_apps;
        } else {
            filtered_apps.clear();
            filtered_apps.reserve(all_apps.size());
            for (const auto& app : all_apps) {
                std::string app_name = app.name;
                std::transform(app_name.begin(), app_name.end(), app_name.begin(), ::tolower);
                if (app_name.find(ql) != std::string::npos) {
                    filtered_apps.push_back(app);
                }
            }
        }
        
        current_page = 0;
        selected_index = filtered_apps.empty() ? -1 : 0;
        refresh_current_view();
        update_app_name_label();
    }

    void refresh_current_view() {
        create_app_buttons();
    }

    void update_app_buttons() {
        create_app_buttons();
    }

    void update_app_name_label() {
        if (!launcher->get_app_name_label()) return;
        const char* text = "";
        if (selected_index >= 0 && selected_index < static_cast<int>(filtered_apps.size())) {
            text = filtered_apps[selected_index].name.c_str();
        }
        gtk_label_set_text(GTK_LABEL(launcher->get_app_name_label()), text);
    }

    void ensure_selection_initialized() {
        if (selected_index < 0 && !filtered_apps.empty()) selected_index = 0;
        if (selected_index >= static_cast<int>(filtered_apps.size())) selected_index = filtered_apps.empty() ? -1 : static_cast<int>(filtered_apps.size()) - 1;
    }

    void update_selection_visuals() {
        ensure_selection_initialized();
        // Clear selection class on buttons
        for (auto* b : app_buttons) {
            GtkStyleContext* ctx = gtk_widget_get_style_context(b);
            gtk_style_context_remove_class(ctx, "selected");
        }
        if (selected_index < 0) { update_app_name_label(); return; }
        
        size_t page_start = current_page * APPS_PER_PAGE;
        if (selected_index >= static_cast<int>(page_start) && selected_index < static_cast<int>(page_start + app_buttons.size())) {
            size_t local = static_cast<size_t>(selected_index) - page_start;
            if (local < app_buttons.size()) {
                GtkStyleContext* ctx = gtk_widget_get_style_context(app_buttons[local]);
                gtk_style_context_add_class(ctx, "selected");
            }
        }
        update_app_name_label();
    }

    void select_next() {
        if (filtered_apps.empty()) return;
        ensure_selection_initialized();
        int max_index = static_cast<int>(filtered_apps.size()) - 1;
        selected_index = std::min(selected_index + 1, max_index);
        size_t new_page = static_cast<size_t>(selected_index) / APPS_PER_PAGE;
        if (new_page != current_page) {
            current_page = new_page;
            refresh_current_view();
        } else {
            update_selection_visuals();
        }
    }

    void select_prev() {
        if (filtered_apps.empty()) return;
        ensure_selection_initialized();
        selected_index = std::max(selected_index - 1, 0);
        size_t new_page = static_cast<size_t>(selected_index) / APPS_PER_PAGE;
        if (new_page != current_page) {
            current_page = new_page;
            refresh_current_view();
        } else {
            update_selection_visuals();
        }
    }

    void activate_selected() {
        if (selected_index < 0 || selected_index >= static_cast<int>(filtered_apps.size())) return;
        AppInfo& app = filtered_apps[selected_index];
        launcher->increment_app_usage(app.name);
        std::string command = app.exec + " &";
        system(command.c_str());
        gtk_main_quit();
    }

    void scroll_up() {
        if (current_page == 0) return;
        current_page--;
        refresh_current_view();
    }

    void scroll_down() {
        size_t total = filtered_apps.size();
        if (total == 0) return;
        size_t max_page = (total - 1) / APPS_PER_PAGE;
        if (current_page < max_page) {
            current_page++;
            refresh_current_view();
        }
    }

    void show_buttons() {
        for (auto* button : app_buttons) {
            gtk_widget_set_visible(button, TRUE);
        }
    }

    void hide_buttons() {
        for (auto* button : app_buttons) {
            gtk_widget_set_visible(button, FALSE);
        }
    }

    void ensure_emojis_loaded() {
        // This method is not used in AppsManager, but needed for interface compatibility
    }

    // Getters
    const std::vector<AppInfo>& get_filtered_apps() const { return filtered_apps; }
    int get_selected_index() const { return selected_index; }
    size_t get_current_page() const { return current_page; }
    const std::vector<GtkWidget*>& get_app_buttons() const { return app_buttons; }
}; 