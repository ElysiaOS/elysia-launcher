#include "ely_launcher.h"
#include <curl/curl.h>
#include <json/json.h>
#include <sstream>
#include <thread>
#include <algorithm>
#include <set>

class GifManager {
private:
    std::vector<GifItem> all_gifs;
    std::vector<GifItem> filtered_gifs;
    std::map<size_t, GdkPixbuf*> loaded_thumbnails;
    std::set<size_t> loading_indices;
    std::vector<GtkWidget*> gif_buttons;
    size_t current_page = 0;
    static constexpr size_t GIFS_PER_PAGE = 7;
    int selected_index = -1;
    EnhancedEdgeLauncher* launcher;
    bool gifs_loaded = false;
    bool loading_gifs = false;
    std::string current_search_query = "";
    guint search_timeout_id = 0;
    int active_downloads = 0;
    static constexpr int MAX_CONCURRENT_DOWNLOADS = 5;
    
    // Tenor API configuration
    static constexpr const char* TENOR_API_KEY = "";
    static constexpr const char* TENOR_BASE_URL = "https://tenor.googleapis.com/v2/search";

public:
    GifManager(EnhancedEdgeLauncher* l) : launcher(l) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~GifManager() {
        for (auto* button : gif_buttons) {
            gtk_widget_destroy(button);
        }
        if (search_timeout_id > 0) {
            g_source_remove(search_timeout_id);
        }
        for (auto& pair : loaded_thumbnails) {
            if (pair.second) g_object_unref(pair.second);
        }
        curl_global_cleanup();
    }

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
    
    static gboolean decrease_downloads_callback(gpointer data) {
        auto* self = static_cast<GifManager*>(data);
        self->active_downloads--;
        return G_SOURCE_REMOVE;
    }

    void fetch_gifs_from_tenor(const std::string& query) {
        if (loading_gifs) return;
        loading_gifs = true;
        
        if (launcher->get_app_name_label()) {
            gtk_label_set_text(GTK_LABEL(launcher->get_app_name_label()), "Loading GIFs...");
        }
        
        all_gifs.clear();
        filtered_gifs.clear();
        for (auto& pair : loaded_thumbnails) {
            if (pair.second) g_object_unref(pair.second);
        }
        loaded_thumbnails.clear();
        loading_indices.clear();
        
        std::thread([this, query]() {
            std::vector<GifItem> new_gifs;
            std::string next_pos = "";
            int page_count = 0;
            const int MAX_PAGES = 5;
            
            while (page_count < MAX_PAGES) {
                CURL* curl = curl_easy_init();
                if (!curl) break;
                
                char* encoded_query = curl_easy_escape(curl, query.c_str(), query.length());
                std::string encoded_query_str = encoded_query ? std::string(encoded_query) : query;
                if (encoded_query) curl_free(encoded_query);
                
                std::string url = std::string(TENOR_BASE_URL) + "?q=" + encoded_query_str + "&key=" + TENOR_API_KEY + "&limit=50&media_filter=minimal&contentfilter=high";
                if (!next_pos.empty()) {
                    url += "&pos=" + next_pos;
                }
                
                std::string response;
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                curl_easy_setopt(curl, CURLOPT_USERAGENT, "Ely-Launcher/1.0");
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                
                CURLcode res = curl_easy_perform(curl);
                bool should_continue = false;
                if (res == CURLE_OK && !response.empty()) {
                    Json::Value root;
                    Json::Reader reader;
                    if (reader.parse(response, root)) {
                        if (root.isMember("next") && !root["next"].asString().empty()) {
                            next_pos = root["next"].asString();
                            should_continue = true;
                        }
                        
                        if (root.isMember("results")) {
                            const Json::Value& results = root["results"];
                            if (results.size() == 0) {
                                should_continue = false;
                            }
                            
                            for (unsigned int idx = 0; idx < results.size(); ++idx) {
                                const Json::Value& result = results[idx];
                                GifItem gif;
                                
                                if (result.isMember("itemurl")) {
                                    gif.url = result["itemurl"].asString();
                                } else {
                                    if (result.isMember("id") && result.isMember("title")) {
                                        std::string id = result["id"].asString();
                                        std::string title = result["title"].asString();
                                        std::string url_title = title;
                                        std::replace(url_title.begin(), url_title.end(), ' ', '-');
                                        std::replace(url_title.begin(), url_title.end(), '_', '-');
                                        url_title.erase(std::remove_if(url_title.begin(), url_title.end(), 
                                            [](char c) { return !std::isalnum(c) && c != '-'; }), url_title.end());
                                        gif.url = "https://tenor.com/view/" + url_title + "-gif-" + id;
                                    }
                                }
                                
                                if (result.isMember("media_formats")) {
                                    const Json::Value& media_formats = result["media_formats"];
                                    if (media_formats.isMember("tinygif") && media_formats["tinygif"].isMember("url")) {
                                        gif.preview_url = media_formats["tinygif"]["url"].asString();
                                    } else if (media_formats.isMember("nanogif") && media_formats["nanogif"].isMember("url")) {
                                        gif.preview_url = media_formats["nanogif"]["url"].asString();
                                    } else if (media_formats.isMember("gif") && media_formats["gif"].isMember("url")) {
                                        gif.preview_url = media_formats["gif"]["url"].asString();
                                    } else if (media_formats.isMember("mediumgif") && media_formats["mediumgif"].isMember("url")) {
                                        gif.preview_url = media_formats["mediumgif"]["url"].asString();
                                    }
                                }
                                
                                if (result.isMember("title")) {
                                    gif.name = result["title"].asString();
                                } else {
                                    gif.name = "GIF #" + std::to_string(new_gifs.size() + 1);
                                }
                                
                                if (result.isMember("id")) {
                                    gif.tenor_id = result["id"].asString();
                                }
                                
                                if (!gif.url.empty() && !gif.preview_url.empty()) {
                                    new_gifs.push_back(gif);
                                }
                            }
                        }
                    }
                }
                
                curl_easy_cleanup(curl);
                if (!should_continue) break;
                page_count++;
            }
            
            g_idle_add([](gpointer data) -> gboolean {
                auto* args = static_cast<std::pair<GifManager*, std::vector<GifItem>>*>(data);
                GifManager* self = args->first;
                self->all_gifs = args->second;
                self->update_gifs_from_thread();
                delete args;
                return G_SOURCE_REMOVE;
            }, new std::pair<GifManager*, std::vector<GifItem>>(this, new_gifs));
        }).detach();
    }

    void update_gifs_from_thread() {
        loading_gifs = false;
        filtered_gifs = all_gifs;
        gifs_loaded = true;
        current_page = 0;
        selected_index = filtered_gifs.empty() ? -1 : 0;
        refresh_current_view();
        update_app_name_label();
    }

    void ensure_gifs_loaded() {
        if (gifs_loaded && !current_search_query.empty()) return;
        
        if (current_search_query.empty()) {
            if (launcher->get_app_name_label()) {
                gtk_label_set_text(GTK_LABEL(launcher->get_app_name_label()), "Search for GIFs...");
            }
            return;
        }
        
        if (!loading_gifs) {
            fetch_gifs_from_tenor(current_search_query);
        }
    }

    void load_visible_thumbnails() {
        if (filtered_gifs.empty()) return;
        
        size_t start_idx = current_page * GIFS_PER_PAGE;
        size_t end_idx = std::min((current_page + 2) * GIFS_PER_PAGE, filtered_gifs.size());
        
        for (size_t i = start_idx; i < end_idx; ++i) {
            if (loaded_thumbnails.find(i) != loaded_thumbnails.end() || 
                loading_indices.find(i) != loading_indices.end()) {
                continue;
            }
            load_gif_thumbnail_for_item(i);
        }
    }

    void load_gif_thumbnail_for_item(size_t gif_index) {
        if (gif_index >= filtered_gifs.size()) return;
        if (loading_indices.find(gif_index) != loading_indices.end()) return;
        
        const GifItem& gif = filtered_gifs[gif_index];
        if (gif.preview_url.empty()) return;
        
        loading_indices.insert(gif_index);
        
        std::thread([this, gif_index, gif]() {
            while (active_downloads >= MAX_CONCURRENT_DOWNLOADS) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            active_downloads++;
            
            CURL* curl = curl_easy_init();
            if (curl) {
                std::string image_data;
                curl_easy_setopt(curl, CURLOPT_URL, gif.preview_url.c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &image_data);
                curl_easy_setopt(curl, CURLOPT_USERAGENT, "Ely-Launcher/1.0");
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                
                CURLcode res = curl_easy_perform(curl);
                long response_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
                
                if (res == CURLE_OK && !image_data.empty() && response_code == 200) {
                    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
                    GError* error = nullptr;
                    
                    if (gdk_pixbuf_loader_write(loader, (const guchar*)image_data.c_str(), image_data.size(), &error)) {
                        if (gdk_pixbuf_loader_close(loader, &error)) {
                            GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
                            if (pixbuf) {
                                g_object_ref(pixbuf);
                                int target_size = launcher->get_config().gif_size;
                                GdkPixbuf* scaled = gdk_pixbuf_scale_simple(pixbuf, target_size, target_size, GDK_INTERP_BILINEAR);
                                if (scaled) {
                                    g_idle_add([](gpointer data) -> gboolean {
                                        auto* args = static_cast<std::tuple<GifManager*, size_t, GdkPixbuf*>*>(data);
                                        GifManager* self = std::get<0>(*args);
                                        size_t index = std::get<1>(*args);
                                        GdkPixbuf* pix = std::get<2>(*args);
                                        
                                        self->loaded_thumbnails[index] = pix;
                                        self->loading_indices.erase(index);
                                        
                                        size_t page_start = self->current_page * self->GIFS_PER_PAGE;
                                        size_t page_end = page_start + self->GIFS_PER_PAGE;
                                        if (index >= page_start && index < page_end) {
                                            self->refresh_current_view();
                                        }
                                        
                                        self->active_downloads--;
                                        delete args;
                                        return G_SOURCE_REMOVE;
                                    }, new std::tuple<GifManager*, size_t, GdkPixbuf*>(this, gif_index, scaled));
                                    
                                    g_object_unref(pixbuf);
                                } else {
                                    g_object_unref(pixbuf);
                                    g_object_unref(loader);
                                    g_idle_add(decrease_downloads_callback, this);
                                    g_idle_add([](gpointer data) -> gboolean {
                                        auto* args = static_cast<std::pair<GifManager*, size_t>*>(data);
                                        args->first->loading_indices.erase(args->second);
                                        delete args;
                                        return G_SOURCE_REMOVE;
                                    }, new std::pair<GifManager*, size_t>(this, gif_index));
                                }
                            } else {
                                g_object_unref(loader);
                                g_idle_add(decrease_downloads_callback, this);
                                g_idle_add([](gpointer data) -> gboolean {
                                    auto* args = static_cast<std::pair<GifManager*, size_t>*>(data);
                                    args->first->loading_indices.erase(args->second);
                                    delete args;
                                    return G_SOURCE_REMOVE;
                                }, new std::pair<GifManager*, size_t>(this, gif_index));
                            }
                        } else {
                            if (error) g_error_free(error);
                            g_object_unref(loader);
                            g_idle_add(decrease_downloads_callback, this);
                            g_idle_add([](gpointer data) -> gboolean {
                                auto* args = static_cast<std::pair<GifManager*, size_t>*>(data);
                                args->first->loading_indices.erase(args->second);
                                delete args;
                                return G_SOURCE_REMOVE;
                            }, new std::pair<GifManager*, size_t>(this, gif_index));
                        }
                    } else {
                        if (error) g_error_free(error);
                        g_object_unref(loader);
                        g_idle_add(decrease_downloads_callback, this);
                        g_idle_add([](gpointer data) -> gboolean {
                            auto* args = static_cast<std::pair<GifManager*, size_t>*>(data);
                            args->first->loading_indices.erase(args->second);
                            delete args;
                            return G_SOURCE_REMOVE;
                        }, new std::pair<GifManager*, size_t>(this, gif_index));
                    }
                } else {
                    g_idle_add(decrease_downloads_callback, this);
                    g_idle_add([](gpointer data) -> gboolean {
                        auto* args = static_cast<std::pair<GifManager*, size_t>*>(data);
                        args->first->loading_indices.erase(args->second);
                        delete args;
                        return G_SOURCE_REMOVE;
                    }, new std::pair<GifManager*, size_t>(this, gif_index));
                }
                curl_easy_cleanup(curl);
            } else {
                g_idle_add(decrease_downloads_callback, this);
                g_idle_add([](gpointer data) -> gboolean {
                    auto* args = static_cast<std::pair<GifManager*, size_t>*>(data);
                    args->first->loading_indices.erase(args->second);
                    delete args;
                    return G_SOURCE_REMOVE;
                }, new std::pair<GifManager*, size_t>(this, gif_index));
            }
        }).detach();
    }

    static void on_gif_clicked(GtkWidget* button, gpointer /*data_unused*/) {
        const char* gif_url = static_cast<const char*>(g_object_get_data(G_OBJECT(button), "gif_url"));
        if (!gif_url) return;
        
        GtkWidget* toplevel = gtk_widget_get_toplevel(button);
        EnhancedEdgeLauncher* self = nullptr;
        if (GTK_IS_WINDOW(toplevel)) {
            self = static_cast<EnhancedEdgeLauncher*>(g_object_get_data(G_OBJECT(toplevel), "launcher"));
        }
        if (!self) {
            GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            gtk_clipboard_set_text(cb, gif_url, -1);
            gtk_clipboard_store(cb);
            g_timeout_add(120, EnhancedEdgeLauncher::delayed_quit_cb, nullptr);
            return;
        }
        self->copy_to_clipboard_and_quit(gif_url);
    }

    void destroy_gif_buttons() {
        for (auto* b : gif_buttons) {
            if (GTK_IS_WIDGET(b)) {
                gtk_widget_destroy(b);
            }
        }
        gif_buttons.clear();
    }

    void create_gif_buttons() {
        destroy_gif_buttons();
        
        if (filtered_gifs.empty()) return;
        
        size_t start_idx = current_page * GIFS_PER_PAGE;
        size_t end_idx = std::min(start_idx + GIFS_PER_PAGE, filtered_gifs.size());
        
        int start_y = 150;
        int button_spacing = 60;
        int button_x = 85;
        int size = launcher->get_config().gif_size;
        
        for (size_t i = start_idx; i < end_idx; ++i) {
            GtkWidget* btn = gtk_button_new_with_label("Loading...");
            gtk_widget_set_size_request(btn, size + 8, size + 8);
            gtk_widget_set_name(btn, "app-button");
            
            if (loaded_thumbnails.find(i) != loaded_thumbnails.end()) {
                GdkPixbuf* pixbuf = loaded_thumbnails[i];
                if (pixbuf) {
                    GtkWidget* image = gtk_image_new_from_pixbuf(pixbuf);
                    if (image) {
                        gtk_button_set_image(GTK_BUTTON(btn), image);
                        gtk_button_set_label(GTK_BUTTON(btn), "");
                        gtk_widget_show(image);
                    }
                }
            }
            
            std::string tooltip = filtered_gifs[i].name + " - Click to copy link to clipboard";
            gtk_widget_set_tooltip_text(btn, tooltip.c_str());
            
            g_object_set_data_full(G_OBJECT(btn), "gif_url", 
                                 g_strdup(filtered_gifs[i].url.c_str()), g_free);
            g_object_set_data(G_OBJECT(btn), "launcher", launcher);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_gif_clicked), nullptr);
            
            int y = start_y + (i - start_idx) * button_spacing;
            gtk_fixed_put(GTK_FIXED(launcher->get_layout()), btn, button_x, y);
            gtk_widget_set_visible(btn, TRUE);
            gtk_widget_show(btn);
            gif_buttons.push_back(btn);
        }
        
        load_visible_thumbnails();
        update_selection_visuals();
    }

    static gboolean debounced_search(gpointer data) {
        auto* args = static_cast<std::pair<GifManager*, std::string>*>(data);
        GifManager* self = args->first;
        std::string query = args->second;
        delete args;
        
        self->search_timeout_id = 0;
        self->perform_search(query);
        return G_SOURCE_REMOVE;
    }
    
    void perform_search(const std::string& query) {
        std::string trimmed_query = query;
        trimmed_query.erase(0, trimmed_query.find_first_not_of(" \t"));
        trimmed_query.erase(trimmed_query.find_last_not_of(" \t") + 1);
        
        if (trimmed_query.length() < 1) {
            filtered_gifs.clear();
            for (auto& pair : loaded_thumbnails) {
                if (pair.second) g_object_unref(pair.second);
            }
            loaded_thumbnails.clear();
            loading_indices.clear();
            current_search_query = "";
            current_page = 0;
            selected_index = -1;
            refresh_current_view();
            update_app_name_label();
            return;
        }
        
        current_search_query = trimmed_query;
        fetch_gifs_from_tenor(trimmed_query);
    }
    
    void filter_gifs(const std::string& query) {
        if (search_timeout_id > 0) {
            g_source_remove(search_timeout_id);
            search_timeout_id = 0;
        }
        
        search_timeout_id = g_timeout_add(300, debounced_search, 
            new std::pair<GifManager*, std::string>(this, query));
    }

    void refresh_current_view() {
        create_gif_buttons();
    }

    void update_app_name_label() {
        if (!launcher->get_app_name_label()) return;
        std::string text;
        if (selected_index >= 0 && selected_index < static_cast<int>(filtered_gifs.size())) {
            text = filtered_gifs[selected_index].name;
        } else if (loading_gifs) {
            text = "Loading GIFs...";
        } else if (!current_search_query.empty() && filtered_gifs.empty()) {
            text = "No GIFs found";
        } else if (!current_search_query.empty()) {
            text = "Found " + std::to_string(filtered_gifs.size()) + " GIFs";
        } else {
            text = "Search for GIFs...";
        }
        gtk_label_set_text(GTK_LABEL(launcher->get_app_name_label()), text.c_str());
    }

    void ensure_selection_initialized() {
        if (selected_index < 0 && !filtered_gifs.empty()) selected_index = 0;
        if (selected_index >= static_cast<int>(filtered_gifs.size())) 
            selected_index = filtered_gifs.empty() ? -1 : static_cast<int>(filtered_gifs.size()) - 1;
    }

    void update_selection_visuals() {
        ensure_selection_initialized();
        for (auto* b : gif_buttons) {
            GtkStyleContext* ctx = gtk_widget_get_style_context(b);
            gtk_style_context_remove_class(ctx, "selected");
        }
        if (selected_index < 0) { update_app_name_label(); return; }
        
        size_t page_start = current_page * GIFS_PER_PAGE;
        if (selected_index >= static_cast<int>(page_start) && 
            selected_index < static_cast<int>(page_start + gif_buttons.size())) {
            size_t local = static_cast<size_t>(selected_index) - page_start;
            if (local < gif_buttons.size()) {
                GtkStyleContext* ctx = gtk_widget_get_style_context(gif_buttons[local]);
                gtk_style_context_add_class(ctx, "selected");
            }
        }
        update_app_name_label();
    }

    void select_next() {
        if (filtered_gifs.empty()) return;
        ensure_selection_initialized();
        int max_index = static_cast<int>(filtered_gifs.size()) - 1;
        selected_index = std::min(selected_index + 1, max_index);
        size_t new_page = static_cast<size_t>(selected_index) / GIFS_PER_PAGE;
        if (new_page != current_page) {
            current_page = new_page;
            refresh_current_view();
        } else {
            update_selection_visuals();
        }
    }

    void select_prev() {
        if (filtered_gifs.empty()) return;
        ensure_selection_initialized();
        selected_index = std::max(selected_index - 1, 0);
        size_t new_page = static_cast<size_t>(selected_index) / GIFS_PER_PAGE;
        if (new_page != current_page) {
            current_page = new_page;
            refresh_current_view();
        } else {
            update_selection_visuals();
        }
    }

    void activate_selected() {
        if (selected_index < 0 || selected_index >= static_cast<int>(filtered_gifs.size())) return;
        const std::string& url = filtered_gifs[selected_index].url;
        launcher->copy_to_clipboard_and_quit(url);
    }

    void scroll_up() {
        if (current_page == 0) return;
        current_page--;
        refresh_current_view();
    }

    void scroll_down() {
        size_t total = filtered_gifs.size();
        if (total == 0) return;
        size_t max_page = (total - 1) / GIFS_PER_PAGE;
        if (current_page < max_page) {
            current_page++;
            refresh_current_view();
        }
    }

    void show_buttons() {
        for (auto* button : gif_buttons) {
            gtk_widget_set_visible(button, TRUE);
        }
    }

    void hide_buttons() {
        for (auto* button : gif_buttons) {
            gtk_widget_set_visible(button, FALSE);
        }
    }

    // Getters
    const std::vector<GifItem>& get_filtered_gifs() const { return filtered_gifs; }
    int get_selected_index() const { return selected_index; }
    size_t get_current_page() const { return current_page; }
    const std::vector<GtkWidget*>& get_gif_buttons() const { return gif_buttons; }
};