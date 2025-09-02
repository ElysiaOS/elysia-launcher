#include "ely_launcher.h"
#include <fstream>
#include <curl/curl.h>

// CURL callback for receiving data
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Forward declarations for the manager classes
class AppsManager;
class EmojiManager;
class GifManager;
class FilesManager;
class WallpaperManager;

// Include the manager implementations
#include "apps.cpp"
#include "emoji.cpp"
#include "gif.cpp"
#include "files.cpp"
#include "wallpaper.cpp"

EnhancedEdgeLauncher::EnhancedEdgeLauncher() {
    window_width = png_width;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Load config with retry mechanism
    load_config();
    // If load_config had errors and restored from backup, we need to reload
    // Check if we have valid config values
    if (config.emoji_size < 20 || config.emoji_size > 200 || 
        config.gif_size < 20 || config.gif_size > 200) {
        printf("DEBUG: Config appears invalid after load, retrying...\n");
        load_config(); // One more try
    }
    
    load_cache();
    detect_theme_variant();
    
    // Initialize managers
    apps_manager = new AppsManager(this);
    emoji_manager = new EmojiManager(this);
    gif_manager = new GifManager(this);
    files_manager = config.files_enabled ? new FilesManager(this) : nullptr;
    wallpaper_manager = new WallpaperManager(this);
    
    create_widget();
    apps_manager->load_applications_async();
}

EnhancedEdgeLauncher::~EnhancedEdgeLauncher() {
    if (animation_timer) {
        g_source_remove(animation_timer);
    }
    if (glitter_timer) {
        g_source_remove(glitter_timer);
    }
    
    if (bg_pixbuf) {
        g_object_unref(bg_pixbuf);
    }
    if (placeholder_pixbuf) {
        g_object_unref(placeholder_pixbuf);
    }
    
    delete apps_manager;
    delete emoji_manager;
    delete gif_manager;
    if (files_manager) delete files_manager;
    delete wallpaper_manager;
    
    curl_global_cleanup();
    
    if (window) {
        gtk_widget_destroy(window);
    }
}

void EnhancedEdgeLauncher::create_widget() {
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Edge Launcher");
    gtk_window_set_default_size(GTK_WINDOW(window), window_width, 400);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(window), TRUE);
    gtk_window_set_focus_on_map(GTK_WINDOW(window), TRUE);

    if (gtk_layer_is_supported()) {
        gtk_layer_init_for_window(GTK_WINDOW(window));
        gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_namespace(GTK_WINDOW(window), "ely-launcher");
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, FALSE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, FALSE);
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, FALSE);
        gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, -500);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    } else {
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
        gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    }

    apply_css();

    layout = gtk_fixed_new();
    gtk_container_add(GTK_CONTAINER(window), layout);
    // Store launcher on window for access in callbacks
    g_object_set_data(G_OBJECT(window), "launcher", this);

    create_placeholder_background();
    bg_image = gtk_image_new_from_pixbuf(placeholder_pixbuf);
    gtk_fixed_put(GTK_FIXED(layout), bg_image, 50, 0);
    load_background_async();

    // Create search entry first
    search_entry = gtk_entry_new();
    gtk_widget_set_name(search_entry, "search-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), "Search...");
    gtk_widget_set_size_request(search_entry, 280, 40);
    gtk_fixed_put(GTK_FIXED(layout), search_entry, 65, 20);

    g_signal_connect(search_entry, "changed", G_CALLBACK(on_search_changed), this);
    g_signal_connect(search_entry, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer self) -> gboolean {
        auto* launcher = static_cast<EnhancedEdgeLauncher*>(self);
        // Navigation keys override entry movement
        if (event->keyval == GDK_KEY_Up) {
            if (launcher->get_current_mode() == ViewMode::Apps) {
                launcher->get_apps_manager()->select_prev();
            } else if (launcher->get_current_mode() == ViewMode::Emojis && launcher->get_config().emoji_enabled) {
                launcher->get_emoji_manager()->select_prev();
            } else if (launcher->get_current_mode() == ViewMode::Gifs && launcher->get_config().gifs_enabled) {
                launcher->get_gif_manager()->select_prev();
            } else if (launcher->get_current_mode() == ViewMode::Files) {
                launcher->get_files_manager()->select_prev();
            } else if (launcher->get_current_mode() == ViewMode::Wallpapers) {
                launcher->get_wallpaper_manager()->select_prev();
            }
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Down) {
            if (launcher->get_current_mode() == ViewMode::Apps) {
                launcher->get_apps_manager()->select_next();
            } else if (launcher->get_current_mode() == ViewMode::Emojis && launcher->get_config().emoji_enabled) {
                launcher->get_emoji_manager()->select_next();
            } else if (launcher->get_current_mode() == ViewMode::Gifs && launcher->get_config().gifs_enabled) {
                launcher->get_gif_manager()->select_next();
            } else if (launcher->get_current_mode() == ViewMode::Files) {
                launcher->get_files_manager()->select_next();
            } else if (launcher->get_current_mode() == ViewMode::Wallpapers) {
                launcher->get_wallpaper_manager()->select_next();
            }
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
            if (launcher->get_current_mode() == ViewMode::Apps) {
                launcher->get_apps_manager()->activate_selected();
            } else if (launcher->get_current_mode() == ViewMode::Emojis && launcher->get_config().emoji_enabled) {
                launcher->get_emoji_manager()->activate_selected();
            } else if (launcher->get_current_mode() == ViewMode::Gifs && launcher->get_config().gifs_enabled) {
                launcher->get_gif_manager()->activate_selected();
            } else if (launcher->get_current_mode() == ViewMode::Files) {
                launcher->get_files_manager()->activate_selected();
            } else if (launcher->get_current_mode() == ViewMode::Wallpapers) {
                launcher->get_wallpaper_manager()->activate_selected();
            }
            return TRUE;
        }
        launcher->spawn_glitter_burst(6);
        return FALSE;
    }), this);

    // Label to display selected app name
    app_name_label = gtk_label_new("");
    gtk_widget_set_name(app_name_label, "app-name-label");
    gtk_widget_set_size_request(app_name_label, 280, 28);
    gtk_label_set_xalign(GTK_LABEL(app_name_label), 0.0);
    gtk_fixed_put(GTK_FIXED(layout), app_name_label, 85, 120);

    // Mode buttons (Apps / Emojis)
    create_mode_buttons();

    // Now create app buttons after entry/label
    apps_manager->create_app_buttons();

    // Glitter overlay on top of everything, non-interactive
    glitter_area = gtk_drawing_area_new();
    gtk_widget_set_name(glitter_area, "glitter-area");
    gtk_widget_set_app_paintable(glitter_area, TRUE);
    gtk_widget_set_size_request(glitter_area, window_width, 400);
    gtk_widget_set_sensitive(glitter_area, FALSE);
    gtk_fixed_put(GTK_FIXED(layout), glitter_area, 0, 0);
    g_signal_connect(glitter_area, "draw", G_CALLBACK(on_glitter_draw), this);
    g_signal_connect(glitter_area, "realize", G_CALLBACK(on_glitter_realize), nullptr);

    gtk_widget_add_events(window, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(window, "button-press-event", G_CALLBACK(on_click_event), this);
    g_signal_connect(window, "scroll-event", G_CALLBACK(on_scroll_event), this);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), this);

    gtk_widget_set_can_focus(window, TRUE);

    gtk_widget_show_all(window);
    buttons_visible = TRUE;

    gtk_widget_grab_focus(search_entry);
    const gchar* no_anim_env = g_getenv("ELY_NO_ANIM");
    bool no_anim = (no_anim_env && g_ascii_strcasecmp(no_anim_env, "0") != 0);
    if (no_anim) {
        gtk_widget_set_opacity(window, 1.0);
        // Keep the current margin; no slide-in
    } else {
        start_opening_animation();
    }
}

void EnhancedEdgeLauncher::create_mode_buttons() {
    // Apps button (always enabled)
    mode_apps_button = gtk_button_new_with_label("Apps");
    gtk_widget_set_name(mode_apps_button, "mode-button");
    gtk_widget_set_size_request(mode_apps_button, 62, 48);
    gtk_fixed_put(GTK_FIXED(layout), mode_apps_button, 0, 110);
    g_signal_connect(mode_apps_button, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer self){
        static_cast<EnhancedEdgeLauncher*>(self)->switch_to_apps();
    }), this);
    
    int button_y = 170;
    
    // Emojis button (conditional)
    if (config.emoji_enabled) {
        mode_emojis_button = gtk_button_new_with_label("Emoji");
        gtk_widget_set_name(mode_emojis_button, "mode-button");
        gtk_widget_set_size_request(mode_emojis_button, 62, 48);
        gtk_fixed_put(GTK_FIXED(layout), mode_emojis_button, 0, button_y);
        g_signal_connect(mode_emojis_button, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer self){
            static_cast<EnhancedEdgeLauncher*>(self)->switch_to_emojis();
        }), this);
        button_y += 60;
    } else {
        mode_emojis_button = nullptr;
    }
    
    // GIFs button (conditional)
    if (config.gifs_enabled) {
        mode_gifs_button = gtk_button_new_with_label("GIFs");
        gtk_widget_set_name(mode_gifs_button, "mode-button");
        gtk_widget_set_size_request(mode_gifs_button, 62, 48);
        gtk_fixed_put(GTK_FIXED(layout), mode_gifs_button, 0, button_y);
        g_signal_connect(mode_gifs_button, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer self){
            static_cast<EnhancedEdgeLauncher*>(self)->switch_to_gifs();
        }), this);
        button_y += 60;
    } else {
        mode_gifs_button = nullptr;
    }

    // Files button (conditional)
    if (config.files_enabled) {
        mode_files_button = gtk_button_new_with_label("File");
        gtk_widget_set_name(mode_files_button, "mode-button");
        gtk_widget_set_size_request(mode_files_button, 62, 48);
        gtk_fixed_put(GTK_FIXED(layout), mode_files_button, 0, button_y);
        g_signal_connect(mode_files_button, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer self){
            static_cast<EnhancedEdgeLauncher*>(self)->switch_to_files();
        }), this);
    } else {
        mode_files_button = nullptr;
    }

    // Start in Apps mode
    GtkStyleContext* a = gtk_widget_get_style_context(mode_apps_button);
    gtk_style_context_add_class(a, "selected");
}

void EnhancedEdgeLauncher::switch_to_apps() {
    current_mode = ViewMode::Apps;
    
    // Clear search entry and reset search state
    gtk_entry_set_text(GTK_ENTRY(search_entry), "");
    apps_manager->filter_apps("");
    selected_index = apps_manager->get_filtered_apps().empty() ? -1 : 0;
    refresh_current_view();
    
    // visual state
    GtkStyleContext* a = gtk_widget_get_style_context(mode_apps_button);
    gtk_style_context_add_class(a, "selected");
    
    if (mode_emojis_button) {
        GtkStyleContext* e = gtk_widget_get_style_context(mode_emojis_button);
        gtk_style_context_remove_class(e, "selected");
    }
    if (mode_gifs_button) {
        GtkStyleContext* g = gtk_widget_get_style_context(mode_gifs_button);
        gtk_style_context_remove_class(g, "selected");
    }
    if (mode_files_button) {
        GtkStyleContext* f = gtk_widget_get_style_context(mode_files_button);
        gtk_style_context_remove_class(f, "selected");
    }
    
    update_app_name_label();
}

void EnhancedEdgeLauncher::switch_to_emojis() {
    if (!config.emoji_enabled || !mode_emojis_button) return;
    
    current_mode = ViewMode::Emojis;
    
    // Clear search entry and reset search state
    gtk_entry_set_text(GTK_ENTRY(search_entry), "");
    emoji_manager->ensure_emojis_loaded();
    emoji_manager->filter_emojis("");
    selected_index = emoji_manager->get_filtered_emojis().empty() ? -1 : 0;
    refresh_current_view();
    
    GtkStyleContext* a = gtk_widget_get_style_context(mode_apps_button);
    gtk_style_context_remove_class(a, "selected");
    
    GtkStyleContext* e = gtk_widget_get_style_context(mode_emojis_button);
    gtk_style_context_add_class(e, "selected");
    
    if (mode_gifs_button) {
        GtkStyleContext* g = gtk_widget_get_style_context(mode_gifs_button);
        gtk_style_context_remove_class(g, "selected");
    }
    if (mode_files_button) {
        GtkStyleContext* f = gtk_widget_get_style_context(mode_files_button);
        gtk_style_context_remove_class(f, "selected");
    }
    update_app_name_label();
}

void EnhancedEdgeLauncher::switch_to_gifs() {
    if (!config.gifs_enabled || !mode_gifs_button) return;
    
    current_mode = ViewMode::Gifs;
    
    // Clear search entry and reset search state
    gtk_entry_set_text(GTK_ENTRY(search_entry), "");
    gif_manager->ensure_gifs_loaded();
    gif_manager->filter_gifs("");
    selected_index = gif_manager->get_filtered_gifs().empty() ? -1 : 0;
    refresh_current_view();
    
    GtkStyleContext* a = gtk_widget_get_style_context(mode_apps_button);
    gtk_style_context_remove_class(a, "selected");
    
    if (mode_emojis_button) {
        GtkStyleContext* e = gtk_widget_get_style_context(mode_emojis_button);
        gtk_style_context_remove_class(e, "selected");
    }
    
    GtkStyleContext* g = gtk_widget_get_style_context(mode_gifs_button);
    gtk_style_context_add_class(g, "selected");
    if (mode_files_button) {
        GtkStyleContext* f = gtk_widget_get_style_context(mode_files_button);
        gtk_style_context_remove_class(f, "selected");
    }
    update_app_name_label();
}

void EnhancedEdgeLauncher::switch_to_files() {
    if (!config.files_enabled || !mode_files_button || !files_manager) return;
    
    current_mode = ViewMode::Files;
    
    // Clear search entry and reset search state
    gtk_entry_set_text(GTK_ENTRY(search_entry), "");
    files_manager->ensure_ready();
    files_manager->filter_files("");
    selected_index = files_manager->get_filtered_entries().empty() ? -1 : 0;
    refresh_current_view();

    GtkStyleContext* a = gtk_widget_get_style_context(mode_apps_button);
    gtk_style_context_remove_class(a, "selected");
    if (mode_emojis_button) {
        GtkStyleContext* e = gtk_widget_get_style_context(mode_emojis_button);
        gtk_style_context_remove_class(e, "selected");
    }
    if (mode_gifs_button) {
        GtkStyleContext* g = gtk_widget_get_style_context(mode_gifs_button);
        gtk_style_context_remove_class(g, "selected");
    }
    if (mode_files_button) {
        GtkStyleContext* f = gtk_widget_get_style_context(mode_files_button);
        gtk_style_context_add_class(f, "selected");
    }
    update_app_name_label();
}

void EnhancedEdgeLauncher::refresh_current_view() {
    if (current_mode == ViewMode::Apps) {
        if (config.emoji_enabled) emoji_manager->destroy_emoji_buttons();
        if (config.gifs_enabled) gif_manager->destroy_gif_buttons();
        if (config.files_enabled && files_manager) files_manager->destroy_file_buttons();
        wallpaper_manager->destroy_wallpaper_buttons();
        apps_manager->update_app_buttons();
    } else if (current_mode == ViewMode::Emojis && config.emoji_enabled) {
        // hide app buttons
        for (auto* b : apps_manager->get_app_buttons()) gtk_widget_set_visible(b, FALSE);
        if (config.gifs_enabled) gif_manager->destroy_gif_buttons();
        if (config.files_enabled && files_manager) files_manager->destroy_file_buttons();
        wallpaper_manager->destroy_wallpaper_buttons();
        emoji_manager->create_emoji_buttons();
    } else if (current_mode == ViewMode::Gifs && config.gifs_enabled) {
        // hide app buttons
        for (auto* b : apps_manager->get_app_buttons()) gtk_widget_set_visible(b, FALSE);
        if (config.emoji_enabled) emoji_manager->destroy_emoji_buttons();
        if (config.files_enabled && files_manager) files_manager->destroy_file_buttons();
        wallpaper_manager->destroy_wallpaper_buttons();
        gif_manager->create_gif_buttons();
    } else if (current_mode == ViewMode::Files && config.files_enabled && files_manager) {
        // hide app buttons
        for (auto* b : apps_manager->get_app_buttons()) gtk_widget_set_visible(b, FALSE);
        if (config.emoji_enabled) emoji_manager->destroy_emoji_buttons();
        if (config.gifs_enabled) gif_manager->destroy_gif_buttons();
        wallpaper_manager->destroy_wallpaper_buttons();
        files_manager->create_file_buttons();
    } else if (current_mode == ViewMode::Wallpapers) {
        // hide app buttons
        for (auto* b : apps_manager->get_app_buttons()) gtk_widget_set_visible(b, FALSE);
        if (config.emoji_enabled) emoji_manager->destroy_emoji_buttons();
        if (config.gifs_enabled) gif_manager->destroy_gif_buttons();
        if (config.files_enabled && files_manager) files_manager->destroy_file_buttons();
        wallpaper_manager->create_wallpaper_buttons();
    }
}

void EnhancedEdgeLauncher::run() {
    gtk_main();
}

std::string EnhancedEdgeLauncher::hex_to_rgba_060(const std::string& hex) {
    auto clamp = [](int v){ return std::max(0, std::min(255, v)); };
    int r=253,g=132,b=203;
    if (hex.size() == 7 && hex[0] == '#') {
        r = clamp(strtol(hex.substr(1,2).c_str(), nullptr, 16));
        g = clamp(strtol(hex.substr(3,2).c_str(), nullptr, 16));
        b = clamp(strtol(hex.substr(5,2).c_str(), nullptr, 16));
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,0.6)", r, g, b);
    return std::string(buf);
}

void EnhancedEdgeLauncher::detect_theme_variant() {
    // Try GSettings first
    GSettings* settings = g_settings_new("org.gnome.desktop.interface");
    gchar* theme = nullptr;
    if (settings) {
        theme = g_settings_get_string(settings, "gtk-theme");
    }
    std::string theme_name = theme ? std::string(theme) : "";
    if (theme) g_free(theme);
    if (settings) g_object_unref(settings);
    // Fallback to env GTK_THEME
    if (theme_name.empty()) {
        const char* env_theme = g_getenv("GTK_THEME");
        if (env_theme) theme_name = env_theme;
    }
    std::string lower = theme_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("elysiaos-hoc") != std::string::npos) {
        theme_variant = ThemeVariant::ElysiaOS_HoC;
        accent_hex = "#7077bd";
        border_hex = "#b1c9ec";
        image_filename = "hocelf.png";
    } else if (lower.find("elysiaos") != std::string::npos) {
        theme_variant = ThemeVariant::ElysiaOS;
        accent_hex = "#FD84CB";
        border_hex = "#FD84CB";
        image_filename = "elfely.png";
    } else {
        theme_variant = ThemeVariant::Other;
        accent_hex = "#FD84CB";
        border_hex = "#FD84CB";
        image_filename = "elfely.png";
    }
}

void EnhancedEdgeLauncher::load_cache() {
    cache_file_path = std::string(g_get_home_dir()) + "/.cache/ely_launcher_cache.json";
    
    // Create cache directory if it doesn't exist
    std::string cache_dir = std::string(g_get_home_dir()) + "/.cache";
    struct stat st = {};
    if (stat(cache_dir.c_str(), &st) == -1) {
        mkdir(cache_dir.c_str(), 0700);
    }
    
    std::ifstream cache_file(cache_file_path);
    if (!cache_file.is_open()) return;
    
    std::string line;
    bool in_data = false;
    
    while (std::getline(cache_file, line)) {
        // Simple JSON parsing for our specific format
        if (line.find("\"data\": {") != std::string::npos) {
            in_data = true;
            continue;
        }
        if (line.find("}") != std::string::npos && in_data) {
            break;
        }
        
        if (in_data && line.find("\":") != std::string::npos) {
            size_t quote1 = line.find("\"");
            size_t quote2 = line.find("\"", quote1 + 1);
            size_t colon = line.find(":", quote2);
            size_t comma = line.find(",");
            
            if (quote1 != std::string::npos && quote2 != std::string::npos && colon != std::string::npos) {
                std::string app_name = line.substr(quote1 + 1, quote2 - quote1 - 1);
                std::string count_str = line.substr(colon + 1, 
                    (comma != std::string::npos ? comma : line.length()) - colon - 1);
                
                // Remove whitespace
                count_str.erase(std::remove_if(count_str.begin(), count_str.end(), ::isspace), count_str.end());
                
                try {
                    int count = std::stoi(count_str);
                    app_usage_cache[app_name] = count;
                } catch (const std::exception&) {
                    // Skip invalid entries
                }
            }
        }
    }
}

void EnhancedEdgeLauncher::save_cache() {
    std::ofstream cache_file(cache_file_path);
    if (!cache_file.is_open()) return;
    
    cache_file << "{\n";
    cache_file << "  \"version\": \"1.0\",\n";
    cache_file << "  \"data\": {\n";
    
    bool first = true;
    for (const auto& pair : app_usage_cache) {
        if (!first) cache_file << ",\n";
        cache_file << "    \"" << pair.first << "\": " << pair.second;
        first = false;
    }
    
    cache_file << "\n  }\n";
    cache_file << "}\n";
    cache_file.close();
}

void EnhancedEdgeLauncher::increment_app_usage(const std::string& app_name) {
    app_usage_cache[app_name]++;
    save_cache();
}

void EnhancedEdgeLauncher::copy_to_clipboard_and_quit(const std::string& text) {
    const char* wl = g_getenv("WAYLAND_DISPLAY");
    bool used_wl_copy = false;
    if (wl) {
        gchar* wl_copy_path = g_find_program_in_path("wl-copy");
        if (wl_copy_path) {
            FILE* pipe = popen("wl-copy", "w");
            if (pipe) {
                fwrite(text.data(), 1, text.size(), pipe);
                fflush(pipe);
                pclose(pipe);
                used_wl_copy = true;
            }
            g_free(wl_copy_path);
        }
    }
    if (!used_wl_copy) {
        GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(cb, text.c_str(), -1);
        gtk_clipboard_store(cb);
    }
    g_timeout_add(used_wl_copy ? 40 : 200, delayed_quit_cb, nullptr);
}

void EnhancedEdgeLauncher::update_app_name_label() {
    if (!app_name_label) return;
    const char* text = "";
    if (current_mode == ViewMode::Apps) {
        if (selected_index >= 0 && selected_index < static_cast<int>(apps_manager->get_filtered_apps().size())) {
            text = apps_manager->get_filtered_apps()[selected_index].name.c_str();
        }
    } else if (current_mode == ViewMode::Emojis) {
        if (selected_index >= 0 && selected_index < static_cast<int>(emoji_manager->get_filtered_emojis().size())) {
            text = emoji_manager->get_filtered_emojis()[selected_index].glyph.c_str();
        }
    } else if (current_mode == ViewMode::Gifs) {
        // GIF label text is managed inside GifManager
    } else if (current_mode == ViewMode::Files) {
        // Files label will show basename
    }
    gtk_label_set_text(GTK_LABEL(app_name_label), text);
}

void EnhancedEdgeLauncher::create_placeholder_background() {
    if (placeholder_pixbuf) return;
    placeholder_pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, png_width, 600);
    gdk_pixbuf_fill(placeholder_pixbuf, 0x444444AA);
}

void EnhancedEdgeLauncher::load_background_async() {
    std::string image_path = image_base_dir + "/" + image_filename;
    if (!g_file_test(image_path.c_str(), G_FILE_TEST_EXISTS)) return;
    std::thread([this, image_path]() {
        GError* error = nullptr;
        GdkPixbuf* loaded = gdk_pixbuf_new_from_file_at_scale(image_path.c_str(), png_width, 600, TRUE, &error);
        if (error) {
            g_error_free(error);
            if (loaded) g_object_unref(loaded);
            return;
        }
        auto* payload = new std::pair<EnhancedEdgeLauncher*, GdkPixbuf*>(this, loaded);
        g_idle_add(apply_bg_pixbuf_idle, payload);
    }).detach();
}

void EnhancedEdgeLauncher::start_opening_animation() {
    animation_progress = 0.0;
    gtk_widget_set_opacity(window, 0.0);
    
    if (gtk_layer_is_supported()) {
        gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, -600);
    }
    
    animation_timer = g_timeout_add(ANIMATION_INTERVAL_MS, animate_opening, this);
}

void EnhancedEdgeLauncher::spawn_glitter_burst(int count) {
    if (!layout) return;
    if (!search_entry || !glitter_area) return;
    GtkAllocation alloc;
    gtk_widget_get_allocation(search_entry, &alloc);
    // Compute caret position inside the entry using Pango layout
    PangoLayout* layout_pl = gtk_entry_get_layout(GTK_ENTRY(search_entry));
    const gchar* txt = gtk_entry_get_text(GTK_ENTRY(search_entry));
    if (!txt) txt = "";
    gint cursor_chars = gtk_editable_get_position(GTK_EDITABLE(search_entry));
    // Clamp cursor to valid range
    glong total_chars = g_utf8_strlen(txt, -1);
    if (cursor_chars < 0) cursor_chars = 0;
    if (cursor_chars > total_chars) cursor_chars = static_cast<gint>(total_chars);
    double caret_x = alloc.x + alloc.width - 10;
    double caret_y = alloc.y + alloc.height / 2.0;
    if (layout_pl && gtk_widget_get_realized(search_entry)) {
        const gchar* cursor_ptr = g_utf8_offset_to_pointer(txt, cursor_chars);
        gint cursor_byte_index = (gint)(cursor_ptr - txt);
        PangoRectangle strong_pos, weak_pos;
        pango_layout_get_cursor_pos(layout_pl, cursor_byte_index, &strong_pos, &weak_pos);
        gint layout_off_x = 0, layout_off_y = 0;
        gtk_entry_get_layout_offsets(GTK_ENTRY(search_entry), &layout_off_x, &layout_off_y);
        caret_x = alloc.x + layout_off_x + strong_pos.x / (double)PANGO_SCALE;
        caret_y = alloc.y + layout_off_y + (strong_pos.y + strong_pos.height / 2.0) / (double)PANGO_SCALE;
    }
    // Jitter around caret inside the entry
    std::uniform_real_distribution<double> offset_x(caret_x - 6.0, caret_x + 6.0);
    std::uniform_real_distribution<double> offset_y(caret_y - 4.0, caret_y + 4.0);
    std::uniform_real_distribution<double> speed(-120.0, -40.0);
    std::uniform_real_distribution<double> angle_spread(-0.8, 0.8);
    std::uniform_real_distribution<double> life_dist(300.0, 800.0);
    std::uniform_real_distribution<double> radius_dist(1.5, 3.5);

    for (int i = 0; i < count; i++) {
        GdkRGBA color = pick_lesbian_palette_color();
        double start_x = offset_x(rng);
        double start_y = offset_y(rng);
        double base_speed = speed(rng);
        double ang = angle_spread(rng);
        GlitterParticle p{
            start_x,
            start_y,
            base_speed * sin(ang) * 0.2,
            base_speed * cos(ang),
            /*life*/ life_dist(rng),
            /*max*/  life_dist(rng),
            radius_dist(rng),
            color
        };
        // Normalize max_life so life <= max_life
        if (p.life_ms > p.max_life_ms) p.max_life_ms = p.life_ms;
        glitter_particles.push_back(p);
    }
    ensure_glitter_timer_running();
    if (glitter_area) gtk_widget_queue_draw(glitter_area);
}

gboolean EnhancedEdgeLauncher::on_glitter_tick() {
    if (glitter_particles.empty()) {
        glitter_timer = 0;
        return G_SOURCE_REMOVE;
    }
    // Advance particles
    const double dt = 16.0; // ms
    const double gravity = 200.0; // px/s^2
    for (auto& p : glitter_particles) {
        double dt_s = dt / 1000.0;
        p.vy += gravity * dt_s * 0.2; // gentle downward pull
        p.x += p.vx * dt_s;
        p.y += p.vy * dt_s;
        p.life_ms -= dt;
    }
    // Remove dead particles
    glitter_particles.erase(
        std::remove_if(glitter_particles.begin(), glitter_particles.end(), [](const GlitterParticle& p){
            return p.life_ms <= 0;
        }),
        glitter_particles.end()
    );
    if (glitter_area) gtk_widget_queue_draw(glitter_area);
    if (glitter_particles.empty()) {
        glitter_timer = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void EnhancedEdgeLauncher::ensure_glitter_timer_running() {
    if (glitter_timer == 0) {
        glitter_timer = g_timeout_add(16, +[](gpointer self) -> gboolean {
            return static_cast<EnhancedEdgeLauncher*>(self)->on_glitter_tick();
        }, this);
    }
}

GdkRGBA EnhancedEdgeLauncher::pick_lesbian_palette_color() {
    static std::vector<GdkRGBA> palette;
    if (palette.empty()) {
        const char* hexes[] = {
            "#D52D00", // dark red-orange
            "#EF7627", // orange
            "#FF9A56", // light orange
            "#FFFFFF", // white
            "#D162A4", // light magenta
            "#B55690", // medium magenta
            "#A30262"  // dark magenta
        };
        for (const char* hx : hexes) {
            GdkRGBA c{};
            gdk_rgba_parse(&c, hx);
            palette.push_back(c);
        }
    }
    std::uniform_int_distribution<size_t> pick(0, palette.size() - 1);
    GdkRGBA c = palette[pick(rng)];
    // slight alpha variance for sparkle depth
    std::uniform_real_distribution<double> a(0.85, 1.0);
    c.alpha = a(rng);
    return c;
}

void EnhancedEdgeLauncher::apply_css() {
    static bool css_applied = false;
    if (css_applied) return;
    
    GtkCssProvider* css_provider = gtk_css_provider_new();
    std::string css = "";
    css += "window { background-color: transparent; border: none; }\n";
    css += "#app-button { background-color: transparent; color: black; border: none; padding: 0; }\n";
    css += "#app-button:hover { background-color: " + accent_hex + "; border-radius: 28px; color: white; }\n";
    css += "#app-button.selected { background-color: " + accent_hex + "; border-radius: 28px; color: white; outline: none; }\n";
    css += "#wallpaper-button { background-color: transparent; color: black; border: none; padding: 0; }\n";
    css += "#wallpaper-button:hover { background-color: " + accent_hex + "; border-radius: 28px; color: white; }\n";
    css += "#wallpaper-button.selected { background-color: " + accent_hex + "; border-radius: 28px; color: white; outline: none; }\n";
    css += "#mode-button { background: rgba(255,255,255,0.35); border: 2px solid " + hex_to_rgba_060(border_hex) + "; border-radius: 28px; font-size: 11px; font-family: ElysiaOSNew12; color: #333; }\n";
    css += "#mode-button.selected { background: " + accent_hex + "; border-radius: 28px; color: white; }\n";
    css += "#search-entry { background: rgba(255,255,255,0.45); border: 2px solid " + hex_to_rgba_060(border_hex) + "; border-radius: 25px; color: #333; font-size: 14px; padding: 12px 20px; }\n";
    css += "#search-entry:focus { border-color: " + accent_hex + "; outline: none; }\n";
    css += "#glitter-area { background-color: transparent; }\n";
    css += "#app-name-label { color: #333; font-size: 16px; font-family: ElysiaOSNew12; background: rgba(255,255,255,0.65); border-radius: 12px; padding: 6px 10px; }\n";

    gtk_css_provider_load_from_data(css_provider, css.c_str(), -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(css_provider);
    css_applied = true;
}

// Static callback implementations
gboolean EnhancedEdgeLauncher::delayed_quit_cb(gpointer) {
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

gboolean EnhancedEdgeLauncher::animate_opening(gpointer data) {
    EnhancedEdgeLauncher* launcher = static_cast<EnhancedEdgeLauncher*>(data);
    
    launcher->animation_progress += (double)ANIMATION_INTERVAL_MS / ANIMATION_DURATION_MS;
    
    if (launcher->animation_progress >= 1.0) {
        launcher->animation_progress = 1.0;
        launcher->animation_timer = 0;
        return G_SOURCE_REMOVE;
    }
    
    double eased_progress = launcher->animation_progress;
    gtk_widget_set_opacity(launcher->window, eased_progress);
    
    if (gtk_layer_is_supported()) {
        int slide_offset = (int)((1.0 - eased_progress) * 100);
        gtk_layer_set_margin(GTK_WINDOW(launcher->window), GTK_LAYER_SHELL_EDGE_RIGHT, -500 - slide_offset);
    }
    
    return G_SOURCE_CONTINUE;
}

gboolean EnhancedEdgeLauncher::on_glitter_draw(GtkWidget* /*widget*/, cairo_t* cr, gpointer data) {
    EnhancedEdgeLauncher* launcher = static_cast<EnhancedEdgeLauncher*>(data);
    if (launcher->glitter_particles.empty()) return FALSE;
    for (const auto& p : launcher->glitter_particles) {
        double t = std::max(0.0, p.life_ms) / std::max(1.0, p.max_life_ms);
        double alpha = t; // fade out
        cairo_set_source_rgba(cr, p.color.red, p.color.green, p.color.blue, alpha);
        cairo_arc(cr, p.x, p.y, p.radius, 0, 2 * G_PI);
        cairo_fill(cr);
        // subtle glow
        cairo_set_source_rgba(cr, p.color.red, p.color.green, p.color.blue, alpha * 0.3);
        cairo_arc(cr, p.x, p.y, p.radius * 2.2, 0, 2 * G_PI);
        cairo_fill(cr);
    }
    return FALSE;
}

void EnhancedEdgeLauncher::on_glitter_realize(GtkWidget* widget, gpointer /*data*/) {
    GdkWindow* win = gtk_widget_get_window(widget);
    if (win) {
        gdk_window_set_pass_through(win, TRUE);
    }
}

gboolean EnhancedEdgeLauncher::on_scroll_event(GtkWidget* /*widget*/, GdkEventScroll* event, gpointer data) {
    EnhancedEdgeLauncher* launcher = static_cast<EnhancedEdgeLauncher*>(data);

    if (!launcher->buttons_visible) {
        launcher->buttons_visible = TRUE;
    }

    if (event->direction == GDK_SCROLL_UP) {
        if (launcher->get_current_mode() == ViewMode::Apps) {
            launcher->get_apps_manager()->scroll_up();
        } else if (launcher->get_current_mode() == ViewMode::Emojis && launcher->get_config().emoji_enabled) {
            launcher->get_emoji_manager()->scroll_up();
        } else if (launcher->get_current_mode() == ViewMode::Gifs && launcher->get_config().gifs_enabled) {
            launcher->get_gif_manager()->scroll_up();
        } else if (launcher->get_current_mode() == ViewMode::Files && launcher->get_config().files_enabled && launcher->get_files_manager()) {
            launcher->get_files_manager()->scroll_up();
        } else if (launcher->get_current_mode() == ViewMode::Wallpapers) {
            launcher->get_wallpaper_manager()->scroll_up();
        }
    } else if (event->direction == GDK_SCROLL_DOWN) {
        if (launcher->get_current_mode() == ViewMode::Apps) {
            launcher->get_apps_manager()->scroll_down();
        } else if (launcher->get_current_mode() == ViewMode::Emojis && launcher->get_config().emoji_enabled) {
            launcher->get_emoji_manager()->scroll_down();
        } else if (launcher->get_current_mode() == ViewMode::Gifs && launcher->get_config().gifs_enabled) {
            launcher->get_gif_manager()->scroll_down();
        } else if (launcher->get_current_mode() == ViewMode::Files && launcher->get_config().files_enabled && launcher->get_files_manager()) {
            launcher->get_files_manager()->scroll_down();
        } else if (launcher->get_current_mode() == ViewMode::Wallpapers) {
            launcher->get_wallpaper_manager()->scroll_down();
        }
    }
    return TRUE;
}

gboolean EnhancedEdgeLauncher::on_click_event(GtkWidget* /*widget*/, GdkEventButton* /*event*/, gpointer data) {
    EnhancedEdgeLauncher* launcher = static_cast<EnhancedEdgeLauncher*>(data);
    if (!launcher->buttons_visible) {
        launcher->buttons_visible = TRUE;
    }
    return TRUE;
}

gboolean EnhancedEdgeLauncher::on_key_press(GtkWidget* /*widget*/, GdkEventKey* event, gpointer data) {
    EnhancedEdgeLauncher* launcher = static_cast<EnhancedEdgeLauncher*>(data);
    
    if (event->keyval == GDK_KEY_Escape) {
        gtk_main_quit();
        return TRUE;
    }
    
    if (event->keyval == GDK_KEY_Up) { 
        if (launcher->get_current_mode() == ViewMode::Apps) {
            launcher->get_apps_manager()->select_prev();
        } else if (launcher->get_current_mode() == ViewMode::Emojis && launcher->get_config().emoji_enabled) {
            launcher->get_emoji_manager()->select_prev();
        } else if (launcher->get_current_mode() == ViewMode::Gifs && launcher->get_config().gifs_enabled) {
            launcher->get_gif_manager()->select_prev();
        } else if (launcher->get_current_mode() == ViewMode::Files && launcher->get_config().files_enabled && launcher->get_files_manager()) {
            launcher->get_files_manager()->select_prev();
        } else if (launcher->get_current_mode() == ViewMode::Wallpapers) {
            launcher->get_wallpaper_manager()->select_prev();
        }
        return TRUE; 
    }
    if (event->keyval == GDK_KEY_Down) { 
        if (launcher->get_current_mode() == ViewMode::Apps) {
            launcher->get_apps_manager()->select_next();
        } else if (launcher->get_current_mode() == ViewMode::Emojis && launcher->get_config().emoji_enabled) {
            launcher->get_emoji_manager()->select_next();
        } else if (launcher->get_current_mode() == ViewMode::Gifs && launcher->get_config().gifs_enabled) {
            launcher->get_gif_manager()->select_next();
        } else if (launcher->get_current_mode() == ViewMode::Files && launcher->get_config().files_enabled && launcher->get_files_manager()) {
            launcher->get_files_manager()->select_next();
        } else if (launcher->get_current_mode() == ViewMode::Wallpapers) {
            launcher->get_wallpaper_manager()->select_next();
        }
        return TRUE; 
    }
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) { 
        if (launcher->get_current_mode() == ViewMode::Apps) {
            launcher->get_apps_manager()->activate_selected();
        } else if (launcher->get_current_mode() == ViewMode::Emojis && launcher->get_config().emoji_enabled) {
            launcher->get_emoji_manager()->activate_selected();
        } else if (launcher->get_current_mode() == ViewMode::Gifs && launcher->get_config().gifs_enabled) {
            launcher->get_gif_manager()->activate_selected();
        } else if (launcher->get_current_mode() == ViewMode::Files && launcher->get_config().files_enabled && launcher->get_files_manager()) {
            launcher->get_files_manager()->activate_selected();
        } else if (launcher->get_current_mode() == ViewMode::Wallpapers) {
            launcher->get_wallpaper_manager()->activate_selected();
        }
        return TRUE; 
    }
    
    if (gtk_widget_has_focus(launcher->search_entry)) { return FALSE; }
    
    if (event->keyval >= 32 && event->keyval <= 126) {
        gtk_widget_grab_focus(launcher->search_entry);
        return FALSE;
    }
    
    return FALSE;
}

void EnhancedEdgeLauncher::on_search_changed(GtkEditable* /*editable*/, gpointer data) {
    EnhancedEdgeLauncher* launcher = static_cast<EnhancedEdgeLauncher*>(data);
    const gchar* search_text = gtk_entry_get_text(GTK_ENTRY(launcher->search_entry));
    std::string query = search_text ? search_text : "";
    
    // Check for wallpaper search - only works in apps mode
    if (launcher->get_current_mode() == ViewMode::Apps && query.length() >= 5 && query.substr(0, 5) == "wall:") {
        std::string wallpaper_query = query.substr(5); // Remove "wall:" prefix
        launcher->set_current_mode(ViewMode::Wallpapers);
        launcher->get_wallpaper_manager()->ensure_ready();
        launcher->get_wallpaper_manager()->filter_wallpapers(wallpaper_query);
        launcher->refresh_current_view();
        launcher->spawn_glitter_burst(10);
        return;
    }
    
    // If we were in wallpaper mode but no longer have "wall:" prefix, switch back to apps
    if (launcher->get_current_mode() == ViewMode::Wallpapers) {
        launcher->set_current_mode(ViewMode::Apps);
        launcher->get_apps_manager()->filter_apps("");
        launcher->refresh_current_view();
    }
    
    // Normalize query lowercase
    std::string ql = query;
    std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);
    
    if (launcher->get_current_mode() == ViewMode::Apps) {
        launcher->get_apps_manager()->filter_apps(ql);
    } else if (launcher->get_current_mode() == ViewMode::Emojis && launcher->get_config().emoji_enabled) {
        launcher->get_emoji_manager()->filter_emojis(ql);
    } else if (launcher->get_current_mode() == ViewMode::Gifs && launcher->get_config().gifs_enabled) {
        launcher->get_gif_manager()->filter_gifs(ql);
    } else if (launcher->get_current_mode() == ViewMode::Files && launcher->get_config().files_enabled && launcher->get_files_manager()) {
        launcher->get_files_manager()->filter_files(query);
    }
    
    launcher->spawn_glitter_burst(10);
}

gboolean EnhancedEdgeLauncher::apply_bg_pixbuf_idle(gpointer data) {
    auto pair = static_cast<std::pair<EnhancedEdgeLauncher*, GdkPixbuf*>*>(data);
    EnhancedEdgeLauncher* self = pair->first;
    GdkPixbuf* loaded = pair->second;
    if (self->bg_pixbuf) {
        g_object_unref(self->bg_pixbuf);
    }
    self->bg_pixbuf = loaded; // takes ownership
    if (self->bg_image) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(self->bg_image), self->bg_pixbuf);
    }
    delete pair;
    return G_SOURCE_REMOVE;
}

void EnhancedEdgeLauncher::create_default_config() {
    // Create config directory if it doesn't exist
    std::string config_dir = std::string(g_get_home_dir()) + "/.config/Elysia/launcher";
    struct stat st = {};
    if (stat(config_dir.c_str(), &st) == -1) {
        mkdir(config_dir.c_str(), 0700);
    }
    
    // Create default config file
    std::string config_path = config_dir + "/ely_launcher.config";
    std::ofstream config_file(config_path);
    if (config_file.is_open()) {
        config_file << "# Ely Launcher Configuration\n";
        config_file << "# Sizes are in pixels\n";
        config_file << "emoji_size: 50\n";
        config_file << "gif_size: 64\n";
        config_file << "# Enable/disable features (true/false)\n";
        config_file << "emoji_enabled: true\n";
        config_file << "gifs_enabled: true\n";
        config_file << "files_enabled: true\n";
        config_file.close();
        printf("DEBUG: Created default config file at %s\n", config_path.c_str());
    }
    
    // Download emoji.txt from ElysiaOS website if it doesn't exist
    std::string emoji_dest = config_dir + "/emoji.txt";
    if (stat(emoji_dest.c_str(), &st) == -1) {
        printf("DEBUG: Downloading emoji.txt from ElysiaOS website...\n");
        
        CURL* curl = curl_easy_init();
        if (curl) {
            std::string response;
            curl_easy_setopt(curl, CURLOPT_URL, "https://www.elysiaos.live/emoji.txt");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Ely-Launcher/1.0");
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK && !response.empty()) {
                std::ofstream emoji_file(emoji_dest);
                if (emoji_file.is_open()) {
                    emoji_file << response;
                    emoji_file.close();
                    printf("DEBUG: Downloaded emoji.txt to %s\n", emoji_dest.c_str());
                } else {
                    printf("DEBUG: Failed to write emoji.txt to %s\n", emoji_dest.c_str());
                }
            } else {
                printf("DEBUG: Failed to download emoji.txt: %s\n", curl_easy_strerror(res));
                
                // Fallback: try to copy from old location if download fails
                std::string emoji_src = std::string(g_get_home_dir()) + "/.config/Elysia/assets/launcher/emoji.txt";
                if (stat(emoji_src.c_str(), &st) == 0) {
                    std::ifstream src(emoji_src, std::ios::binary);
                    std::ofstream dst(emoji_dest, std::ios::binary);
                    if (src && dst) {
                        dst << src.rdbuf();
                        printf("DEBUG: Copied emoji.txt from fallback location to %s\n", emoji_dest.c_str());
                    }
                }
            }
            curl_easy_cleanup(curl);
        } else {
            printf("DEBUG: Failed to initialize CURL for emoji download\n");
        }
    } else {
        printf("DEBUG: emoji.txt already exists at %s\n", emoji_dest.c_str());
    }
}

bool EnhancedEdgeLauncher::validate_config() {
    // Check if all required values are within valid ranges
    bool valid = true;
    
    // Validate emoji_size (should be between 20 and 200)
    if (config.emoji_size < 20 || config.emoji_size > 200) {
        printf("WARNING: Invalid emoji_size: %d (should be 20-200), resetting to default\n", config.emoji_size);
        config.emoji_size = 50;
        valid = false;
    }
    
    // Validate gif_size (should be between 20 and 200)
    if (config.gif_size < 20 || config.gif_size > 200) {
        printf("WARNING: Invalid gif_size: %d (should be 20-200), resetting to default\n", config.gif_size);
        config.gif_size = 64;
        valid = false;
    }
    
    // Boolean values are already validated by the parser
    // (they default to false if not "true")
    
    return valid;
}

void EnhancedEdgeLauncher::backup_config() {
    std::string backup_path = config.config_path + ".backup";
    std::ifstream src(config.config_path, std::ios::binary);
    std::ofstream dst(backup_path, std::ios::binary);
    
    if (src && dst) {
        dst << src.rdbuf();
        printf("DEBUG: Created config backup at %s\n", backup_path.c_str());
    } else {
        printf("WARNING: Failed to create config backup\n");
    }
}

void EnhancedEdgeLauncher::restore_config() {
    std::string backup_path = config.config_path + ".backup";
    std::ifstream src(backup_path, std::ios::binary);
    std::ofstream dst(config.config_path, std::ios::binary);
    
    if (src && dst) {
        dst << src.rdbuf();
        src.close();
        dst.close();
        printf("DEBUG: Restored config from backup\n");
        
        // Validate the restored config
        std::ifstream test_file(config.config_path);
        if (test_file.is_open()) {
            test_file.seekg(0, std::ios::end);
            if (test_file.tellg() == 0) {
                printf("WARNING: Restored backup is empty, creating fresh default\n");
                create_default_config();
            }
            test_file.close();
        } else {
            printf("WARNING: Cannot read restored config, creating fresh default\n");
            create_default_config();
        }
    } else {
        printf("WARNING: Failed to restore config from backup, creating fresh default\n");
        create_default_config();
    }
}

void EnhancedEdgeLauncher::load_config() {
    config.config_path = std::string(g_get_home_dir()) + "/.config/Elysia/launcher/ely_launcher.config";
    
    std::ifstream config_file(config.config_path);
    if (!config_file.is_open()) {
        printf("DEBUG: Config file not found, creating default\n");
        create_default_config();
        return;
    }
    
    // Check if file is empty
    config_file.seekg(0, std::ios::end);
    if (config_file.tellg() == 0) {
        printf("WARNING: Config file is empty, creating default\n");
        config_file.close();
        create_default_config();
        return;
    }
    config_file.seekg(0, std::ios::beg);
    
    // Store original config values for validation
    LauncherConfig original_config = config;
    
    // Track which values were found in the config file
    std::set<std::string> found_keys;
    bool has_errors = false;
    
    std::string line;
    int line_number = 0;
    while (std::getline(config_file, line)) {
        line_number++;
        
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;
        
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);
            
            // Trim whitespace
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            found_keys.insert(key);
            
            if (key == "emoji_size") {
                try {
                    config.emoji_size = std::stoi(value);
                } catch (const std::exception& e) {
                    printf("ERROR: Invalid emoji_size value '%s' at line %d: %s\n", value.c_str(), line_number, e.what());
                    has_errors = true;
                }
            } else if (key == "gif_size") {
                try {
                    config.gif_size = std::stoi(value);
                } catch (const std::exception& e) {
                    printf("ERROR: Invalid gif_size value '%s' at line %d: %s\n", value.c_str(), line_number, e.what());
                    has_errors = true;
                }
            } else if (key == "emoji_enabled") {
                config.emoji_enabled = (value == "true");
            } else if (key == "gifs_enabled") {
                config.gifs_enabled = (value == "true");
            } else if (key == "files_enabled") {
                config.files_enabled = (value == "true");
            } else {
                printf("WARNING: Unknown config key '%s' at line %d\n", key.c_str(), line_number);
            }
        } else {
            printf("WARNING: Invalid config line %d (missing colon): %s\n", line_number, line.c_str());
            has_errors = true;
        }
    }
    
    // Check if all required keys are present
    std::set<std::string> required_keys = {"emoji_size", "gif_size", "emoji_enabled", "gifs_enabled", "files_enabled"};
    std::set<std::string> missing_keys;
    
    for (const auto& key : required_keys) {
        if (found_keys.find(key) == found_keys.end()) {
            missing_keys.insert(key);
        }
    }
    
    if (!missing_keys.empty()) {
        printf("WARNING: Missing required config keys: ");
        for (const auto& key : missing_keys) {
            printf("%s ", key.c_str());
        }
        printf("\n");
        has_errors = true;
    }
    
    // Validate the loaded config
    if (!validate_config()) {
        has_errors = true;
    }
    
    // If there were errors, try to restore from backup or create fresh config
    if (has_errors) {
        printf("ERROR: Config file has errors, attempting to restore from backup...\n");
        
        // First, backup the current (corrupted) config
        backup_config();
        
        // Try to restore from backup
        restore_config();
        
        // Don't recursively call load_config() to avoid infinite loops
        // Instead, just return and let the caller handle it
        return;
    }
    
    printf("DEBUG: Loaded config - emoji_size: %d, gif_size: %d, emoji_enabled: %s, gifs_enabled: %s, files_enabled: %s\n",
           config.emoji_size, config.gif_size, 
           config.emoji_enabled ? "true" : "false",
           config.gifs_enabled ? "true" : "false",
           config.files_enabled ? "true" : "false");
    
    // Print config summary
    printf("DEBUG: Config validation complete - ");
    if (config.emoji_enabled) printf("Emoji: ON(%dpx) ", config.emoji_size);
    else printf("Emoji: OFF ");
    if (config.gifs_enabled) printf("GIFs: ON(%dpx) ", config.gif_size);
    else printf("GIFs: OFF ");
    if (config.files_enabled) printf("Files: ON ");
    else printf("Files: OFF ");
    printf("\n");
}

void EnhancedEdgeLauncher::save_config() {
    // Create backup of current config before saving
    backup_config();
    
    std::ofstream config_file(config.config_path);
    if (config_file.is_open()) {
        config_file << "# Ely Launcher Configuration\n";
        config_file << "# Sizes are in pixels\n";
        config_file << "emoji_size: " << config.emoji_size << "\n";
        config_file << "gif_size: " << config.gif_size << "\n";
        config_file << "# Enable/disable features (true/false)\n";
        config_file << "emoji_enabled: " << (config.emoji_enabled ? "true" : "false") << "\n";
        config_file << "gifs_enabled: " << (config.gifs_enabled ? "true" : "false") << "\n";
        config_file << "files_enabled: " << (config.files_enabled ? "true" : "false") << "\n";
        config_file.close();
        printf("DEBUG: Saved config to %s\n", config.config_path.c_str());
    } else {
        printf("ERROR: Failed to save config to %s\n", config.config_path.c_str());
    }
}

void EnhancedEdgeLauncher::update_emoji_file() {
    std::string config_dir = std::string(g_get_home_dir()) + "/.config/Elysia/launcher";
    std::string emoji_dest = config_dir + "/emoji.txt";
    
    printf("DEBUG: Updating emoji.txt from ElysiaOS website...\n");
    
    CURL* curl = curl_easy_init();
    if (curl) {
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, "https://www.elysiaos.live/emoji.txt");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Ely-Launcher/1.0");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK && !response.empty()) {
            std::ofstream emoji_file(emoji_dest);
            if (emoji_file.is_open()) {
                emoji_file << response;
                emoji_file.close();
                printf("DEBUG: Updated emoji.txt at %s\n", emoji_dest.c_str());
            } else {
                printf("DEBUG: Failed to write emoji.txt to %s\n", emoji_dest.c_str());
            }
        } else {
            printf("DEBUG: Failed to update emoji.txt: %s\n", curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
    } else {
        printf("DEBUG: Failed to initialize CURL for emoji update\n");
    }
}

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    EnhancedEdgeLauncher launcher;
    launcher.run();
    return 0;
} 