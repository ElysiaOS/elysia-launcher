#include "ely_launcher.h"
#include <regex>
#include <sys/stat.h>
#include <thread>

class EmojiManager {
private:
    std::vector<EmojiItem> all_emojis;
    std::vector<EmojiItem> filtered_emojis;
    std::vector<GtkWidget*> emoji_buttons;
    size_t current_page = 0;
    static constexpr size_t EMOJIS_PER_PAGE = 7;
    int selected_index = -1;
    EnhancedEdgeLauncher* launcher;
    bool emojis_loaded = false;
    bool loading_emojis = false;

    // Removed WebResponse struct - no longer needed

public:
    EmojiManager(EnhancedEdgeLauncher* l) : launcher(l) {}

    ~EmojiManager() {
        for (auto* button : emoji_buttons) {
            gtk_widget_destroy(button);
        }
    }

    // Removed CURL functions - no longer needed

    // Removed complex HTML parsing - now using emoji.txt file instead
    
    // Removed URL context extraction - no longer needed
    
    // Generate search keywords from emoji name
    std::string generate_search_keywords(const std::string& name) {
        std::string keywords = name;
        
        // Convert to lowercase
        std::transform(keywords.begin(), keywords.end(), keywords.begin(), ::tolower);
        
        // Replace common words with synonyms
        std::map<std::string, std::string> synonyms = {
            {"face", "face"},
            {"crying", "cry sad tears"},
            {"laughing", "laugh happy joy"},
            {"smiling", "smile happy"},
            {"angry", "mad angry rage"},
            {"sad", "sad cry tears"},
            {"happy", "happy joy smile"},
            {"love", "love heart"},
            {"heart", "heart love"},
            {"food", "food eat"},
            {"drink", "drink beverage"},
            {"animal", "animal pet"},
            {"person", "person human"},
            {"object", "object thing"},
            {"symbol", "symbol sign"},
            {"flag", "flag country"},
            {"plant", "plant nature"},
            {"weather", "weather climate"},
            {"activity", "activity sport"},
            {"emotion", "emotion feeling"}
        };
        
        // Add synonyms for common words
        for (const auto& pair : synonyms) {
            size_t pos = keywords.find(pair.first);
            if (pos != std::string::npos) {
                keywords += " " + pair.second;
            }
        }
        
        // Add individual words as separate keywords
        std::string individual_words = keywords;
        std::replace(individual_words.begin(), individual_words.end(), '-', ' ');
        std::replace(individual_words.begin(), individual_words.end(), '_', ' ');
        
        keywords += " " + individual_words;
        
        return keywords;
    }
    
    // Remove old hardcoded function - now using generate_search_keywords instead

    std::string extract_keywords_from_url(const std::string& url) {
        // Convert URL like "/face-with-tears-of-joy" to keywords "face tears joy"
        std::string keywords = url;
        
        // Remove leading slash and path parts
        size_t last_slash = keywords.find_last_of('/');
        if (last_slash != std::string::npos) {
            keywords = keywords.substr(last_slash + 1);
        }
        
        // Replace hyphens with spaces
        std::replace(keywords.begin(), keywords.end(), '-', ' ');
        
        // Remove common words that aren't useful for search
        std::regex common_words("\\b(emoji|symbol|sign|the|and|or|of|with|in|on|at|to|for|by)\\b");
        keywords = std::regex_replace(keywords, common_words, "");
        
        // Clean up extra spaces
        std::regex extra_spaces("\\s+");
        keywords = std::regex_replace(keywords, extra_spaces, " ");
        
        // Trim
        keywords.erase(0, keywords.find_first_not_of(" \t"));
        keywords.erase(keywords.find_last_not_of(" \t") + 1);
        
        return keywords;
    }

    void load_emojis_from_web() {
        if (loading_emojis) return;
        loading_emojis = true;

        printf("DEBUG: load_emojis_from_web called\n");
        
        all_emojis.clear();
        
        // Load emojis from the local emoji.txt file
        std::string emoji_path = std::string(g_get_home_dir()) + "/.config/Elysia/launcher/emoji.txt";
        
        // Check if emoji.txt exists, if not, try to download it
        struct stat st = {};
        if (stat(emoji_path.c_str(), &st) == -1) {
            printf("DEBUG: emoji.txt not found, attempting to download...\n");
            
            // Download in a separate thread to avoid blocking the UI
            std::thread([this]() {
                launcher->update_emoji_file();
                
                // Retry loading emojis after download completes
                g_idle_add([](gpointer data) -> gboolean {
                    auto* self = static_cast<EmojiManager*>(data);
                    self->load_emojis_from_web();
                    return G_SOURCE_REMOVE;
                }, this);
            }).detach();
            
            // For now, use fallback emojis while downloading
            load_fallback_emojis();
            filtered_emojis = all_emojis;
            emojis_loaded = true;
            loading_emojis = false;
            return;
        }
        
        std::ifstream emoji_file(emoji_path);
        if (emoji_file.is_open()) {
            printf("DEBUG: Loading emojis from emoji.txt\n");
            std::string line;
            while (std::getline(emoji_file, line)) {
                if (!line.empty()) {
                    size_t comma_pos = line.find(',');
                    if (comma_pos != std::string::npos) {
                        std::string emoji = line.substr(0, comma_pos);
                        std::string name = line.substr(comma_pos + 1);
                        
                        // Trim whitespace from name
                        name.erase(0, name.find_first_not_of(" \t"));
                        name.erase(name.find_last_not_of(" \t") + 1);
                        
                        // Generate search keywords from the name
                        std::string keywords = generate_search_keywords(name);
                        
                        all_emojis.push_back(EmojiItem{emoji, keywords});
                        printf("DEBUG: Loaded emoji: %s with name: %s\n", emoji.c_str(), name.c_str());
                    }
                }
            }
            emoji_file.close();
            printf("DEBUG: Loaded %zu emojis from emoji.txt\n", all_emojis.size());
        } else {
            printf("DEBUG: Could not open emoji.txt, using fallback\n");
            load_fallback_emojis();
        }
        
        filtered_emojis = all_emojis;
        emojis_loaded = true;
        loading_emojis = false;
        
        printf("DEBUG: Total emojis loaded: %zu\n", all_emojis.size());
    }
    
    void load_fallback_emojis() {
        // Fallback to a small set of common emojis if web fetch fails
        std::vector<std::pair<std::string, std::string>> fallback_emojis = {
            {"😀", "smile happy face grinning"},
            {"😁", "smile beaming happy"},
            {"😂", "joy tears lol"},
            {"🤣", "rofl rolling laugh"},
            {"😃", "smile open mouth"},
            {"😄", "smile happy"},
            {"😅", "smile sweat"},
            {"😆", "laugh xd"},
            {"😉", "wink"},
            {"😊", "blush smile"},
            {"😋", "yum tasty"},
            {"😎", "cool shades"},
            {"😍", "heart eyes love"},
            {"😘", "kiss"},
            {"🥰", "hearts love smiling"},
            {"🙂", "slight smile"},
            {"🤗", "hug"},
            {"🤩", "star struck"},
            {"🤔", "think thinking"},
            {"😐", "neutral"},
            {"😑", "expressionless"},
            {"🙄", "eyeroll roll"},
            {"😏", "smirk"},
            {"😮", "surprised open mouth wow"},
            {"😪", "sleepy"},
            {"😫", "tired"},
            {"🥱", "yawn"},
            {"😴", "sleep"},
            {"😌", "relieved"},
            {"😛", "tongue"},
            {"😜", "winking tongue"},
            {"🤤", "drool"},
            {"😒", "unamused"},
            {"😓", "sweat sad"},
            {"😔", "pensive"},
            {"😕", "confused"},
            {"🙃", "upside down"},
            {"👍", "thumbs up like"},
            {"👎", "thumbs down dislike"},
            {"👏", "clap applause"},
            {"🙌", "hooray raise hands"},
            {"🙏", "pray please thanks"},
            {"👌", "ok perfect"},
            {"🤘", "rock on"},
            {"✌️", "peace victory"},
            {"🤞", "crossed fingers luck"},
            {"🤟", "love you hand"},
            {"🤙", "call me"},
            {"🤏", "pinch small"},
            {"🤌", "pinched fingers"},
            {"🤝", "handshake deal"},
            {"❤️", "red heart love"},
            {"🧡", "orange heart"},
            {"💛", "yellow heart"},
            {"💚", "green heart"},
            {"💙", "blue heart"},
            {"💜", "purple heart"},
            {"🖤", "black heart"},
            {"🤍", "white heart"},
            {"🤎", "brown heart"},
            {"💖", "sparkling heart"},
            {"💘", "cupid heart"},
            {"💝", "gift heart"},
            {"💞", "revolving hearts"},
            {"💓", "beating heart"},
            {"💗", "growing heart"},
            {"💕", "two hearts"},
            {"🐶", "dog puppy"},
            {"🐱", "cat kitty"},
            {"🐭", "mouse"},
            {"🐹", "hamster"},
            {"🐰", "rabbit bunny"},
            {"🦊", "fox"},
            {"🐻", "bear"},
            {"🐼", "panda"},
            {"🐨", "koala"},
            {"🐯", "tiger"},
            {"🦁", "lion"},
            {"🐮", "cow"},
            {"🐷", "pig"},
            {"🐸", "frog"},
            {"🐵", "monkey"},
            {"🍎", "apple red"},
            {"🍊", "orange fruit"},
            {"🍋", "lemon"},
            {"🍌", "banana"},
            {"🍉", "watermelon"},
            {"🍇", "grapes"},
            {"🍓", "strawberry"},
            {"🍒", "cherries"},
            {"🍑", "peach"},
            {"🥭", "mango"},
            {"🍍", "pineapple"},
            {"🥥", "coconut"},
            {"🥝", "kiwi"},
            {"⚽", "soccer football"},
            {"🏀", "basketball"},
            {"🏈", "american football"},
            {"⚾", "baseball"},
            {"🎾", "tennis"},
            {"🏐", "volleyball"},
            {"🏉", "rugby"},
            {"🎱", "billiards eight ball"},
            {"🚗", "car"},
            {"🚕", "taxi"},
            {"🚙", "suv"},
            {"🚌", "bus"},
            {"🚎", "trolleybus"},
            {"🏎️", "race car"},
            {"🚓", "police car"},
            {"🚑", "ambulance"},
            {"🚒", "fire engine"},
            {"🚐", "minibus"},
            {"🛻", "pickup truck"},
            {"🚚", "delivery truck"},
            {"🚛", "articulated lorry"},
            {"✈️", "airplane plane"},
            {"🚀", "rocket"},
            {"🛸", "ufo"},
            {"🚁", "helicopter"},
            {"🚂", "train locomotive"},
            {"⌚", "watch"},
            {"📱", "phone mobile"},
            {"💻", "laptop computer"},
            {"⌨️", "keyboard"},
            {"🖥️", "desktop computer"},
            {"🖨️", "printer"},
            {"🖱️", "mouse computer"},
            {"🎧", "headphones"},
            {"🎤", "microphone"},
            {"🎹", "piano keyboard"},
            {"🎷", "saxophone"},
            {"🎺", "trumpet"},
            {"🎸", "guitar"},
            {"🎻", "violin"},
            {"⭐", "star"},
            {"🌟", "glowing star"},
            {"✨", "sparkles"},
            {"🔥", "fire"},
            {"💧", "droplet water"},
            {"🌈", "rainbow"},
            {"❄️", "snowflake"},
            {"☀️", "sun"},
            {"🌙", "moon"},
            {"☁️", "cloud"}
        };
        
        for (const auto& emoji : fallback_emojis) {
            all_emojis.push_back(EmojiItem{emoji.first, emoji.second});
        }
        
        printf("DEBUG: Loaded %zu fallback emojis\n", fallback_emojis.size());
    }

    void ensure_emojis_loaded() {
        if (emojis_loaded || loading_emojis) return;
        
        printf("DEBUG: ensure_emojis_loaded called\n");
        
        // Show loading message
        if (launcher->get_app_name_label()) {
            gtk_label_set_text(GTK_LABEL(launcher->get_app_name_label()), "Loading emojis...");
        }
        
        // Load immediately in main thread for now to debug
        load_emojis_from_web();
        refresh_current_view();
        
        printf("DEBUG: ensure_emojis_loaded completed\n");
    }

    static void on_emoji_clicked(GtkWidget* button, gpointer /*data_unused*/) {
        const char* emoji = static_cast<const char*>(g_object_get_data(G_OBJECT(button), "emoji"));
        if (!emoji) return;
        
        GtkWidget* toplevel = gtk_widget_get_toplevel(button);
        EnhancedEdgeLauncher* self = nullptr;
        if (GTK_IS_WINDOW(toplevel)) {
            self = static_cast<EnhancedEdgeLauncher*>(g_object_get_data(G_OBJECT(toplevel), "launcher"));
        }
        if (!self) {
            GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            gtk_clipboard_set_text(cb, emoji, -1);
            gtk_clipboard_store(cb);
            g_timeout_add(120, EnhancedEdgeLauncher::delayed_quit_cb, nullptr);
            return;
        }
        self->copy_to_clipboard_and_quit(emoji);
    }

    void destroy_emoji_buttons() {
        for (auto* b : emoji_buttons) gtk_widget_destroy(b);
        emoji_buttons.clear();
    }

    void create_emoji_buttons() {
        destroy_emoji_buttons();
        
        printf("DEBUG: create_emoji_buttons called, emojis_loaded: %d, filtered_emojis.size: %zu\n", 
               emojis_loaded, filtered_emojis.size());
        
        if (!emojis_loaded) {
            printf("DEBUG: Emojis not loaded yet, calling ensure_emojis_loaded\n");
            ensure_emojis_loaded();
            return;
        }
        
        size_t start_idx = current_page * EMOJIS_PER_PAGE;
        size_t end_idx = std::min(start_idx + EMOJIS_PER_PAGE, filtered_emojis.size());
        
        printf("DEBUG: Creating buttons from %zu to %zu (total: %zu)\n", start_idx, end_idx, filtered_emojis.size());
        
        int start_y = 150;
        int button_spacing = 60;
        int button_x = 85;
        int size = launcher->get_config().emoji_size;
        
        for (size_t i = start_idx; i < end_idx; ++i) {
            printf("DEBUG: Creating button for emoji: %s (length: %zu)\n", 
                   filtered_emojis[i].glyph.c_str(), filtered_emojis[i].glyph.length());
            
            GtkWidget* btn = gtk_button_new_with_label(filtered_emojis[i].glyph.c_str());
            gtk_widget_set_size_request(btn, size + 8, size + 8);
            gtk_widget_set_name(btn, "app-button");
            
            // Create tooltip with emoji name/keywords
            std::string tooltip;
            if (!filtered_emojis[i].keywords.empty()) {
                // Extract the first part as the name (before any synonyms)
                size_t first_space = filtered_emojis[i].keywords.find(' ');
                std::string name = (first_space != std::string::npos) ? 
                    filtered_emojis[i].keywords.substr(0, first_space) : 
                    filtered_emojis[i].keywords;
                
                // Capitalize first letter
                if (!name.empty()) {
                    name[0] = std::toupper(name[0]);
                }
                
                tooltip = name + " - Click to copy to clipboard";
            } else {
                tooltip = "Click to copy to clipboard";
            }
            gtk_widget_set_tooltip_text(btn, tooltip.c_str());
            
            g_object_set_data_full(G_OBJECT(btn), "emoji", 
                                 g_strdup(filtered_emojis[i].glyph.c_str()), g_free);
            g_object_set_data(G_OBJECT(btn), "launcher", launcher);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_emoji_clicked), nullptr);
            
            int y = start_y + (i - start_idx) * button_spacing;
            printf("DEBUG: Placing button at (%d, %d)\n", button_x, y);
            gtk_fixed_put(GTK_FIXED(launcher->get_layout()), btn, button_x, y);
            gtk_widget_set_visible(btn, TRUE);
            gtk_widget_show(btn); // Force show the button
            emoji_buttons.push_back(btn);
        }
        
        printf("DEBUG: Created %zu emoji buttons\n", emoji_buttons.size());
        update_selection_visuals();
    }

    void filter_emojis(const std::string& query) {
        printf("DEBUG: filter_emojis called with query: '%s'\n", query.c_str());
        
        if (!emojis_loaded) {
            printf("DEBUG: Emojis not loaded, calling ensure_emojis_loaded\n");
            ensure_emojis_loaded();
            return;
        }
        
        if (query.empty()) {
            filtered_emojis = all_emojis;
            printf("DEBUG: Empty query, showing all %zu emojis\n", all_emojis.size());
        } else {
            filtered_emojis.clear();
            filtered_emojis.reserve(all_emojis.size());
            
            std::string lower_query = query;
            std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);
            
            for (const auto& e : all_emojis) {
                std::string keywords = e.keywords;
                std::transform(keywords.begin(), keywords.end(), keywords.begin(), ::tolower);
                
                // Check if query matches keywords or emoji itself
                if (keywords.find(lower_query) != std::string::npos || 
                    e.glyph.find(query) != std::string::npos) {
                    filtered_emojis.push_back(e);
                    printf("DEBUG: Matched emoji: %s (keywords: %s)\n", e.glyph.c_str(), e.keywords.c_str());
                }
            }
            printf("DEBUG: Query '%s' found %zu matching emojis\n", query.c_str(), filtered_emojis.size());
        }
        
        current_page = 0;
        selected_index = filtered_emojis.empty() ? -1 : 0;
        refresh_current_view();
        update_app_name_label();
    }

    void refresh_current_view() {
        create_emoji_buttons();
    }

    void update_app_name_label() {
        if (!launcher->get_app_name_label()) return;
        const char* text = "";
        if (selected_index >= 0 && selected_index < static_cast<int>(filtered_emojis.size())) {
            text = filtered_emojis[selected_index].glyph.c_str();
        }
        gtk_label_set_text(GTK_LABEL(launcher->get_app_name_label()), text);
    }

    void ensure_selection_initialized() {
        if (selected_index < 0 && !filtered_emojis.empty()) selected_index = 0;
        if (selected_index >= static_cast<int>(filtered_emojis.size())) 
            selected_index = filtered_emojis.empty() ? -1 : static_cast<int>(filtered_emojis.size()) - 1;
    }

    void update_selection_visuals() {
        ensure_selection_initialized();
        for (auto* b : emoji_buttons) {
            GtkStyleContext* ctx = gtk_widget_get_style_context(b);
            gtk_style_context_remove_class(ctx, "selected");
        }
        if (selected_index < 0) { update_app_name_label(); return; }
        
        size_t page_start = current_page * EMOJIS_PER_PAGE;
        if (selected_index >= static_cast<int>(page_start) && 
            selected_index < static_cast<int>(page_start + emoji_buttons.size())) {
            size_t local = static_cast<size_t>(selected_index) - page_start;
            if (local < emoji_buttons.size()) {
                GtkStyleContext* ctx = gtk_widget_get_style_context(emoji_buttons[local]);
                gtk_style_context_add_class(ctx, "selected");
            }
        }
        update_app_name_label();
    }

    void select_next() {
        if (filtered_emojis.empty()) return;
        ensure_selection_initialized();
        int max_index = static_cast<int>(filtered_emojis.size()) - 1;
        selected_index = std::min(selected_index + 1, max_index);
        size_t new_page = static_cast<size_t>(selected_index) / EMOJIS_PER_PAGE;
        if (new_page != current_page) {
            current_page = new_page;
            refresh_current_view();
        } else {
            update_selection_visuals();
        }
    }

    void select_prev() {
        if (filtered_emojis.empty()) return;
        ensure_selection_initialized();
        selected_index = std::max(selected_index - 1, 0);
        size_t new_page = static_cast<size_t>(selected_index) / EMOJIS_PER_PAGE;
        if (new_page != current_page) {
            current_page = new_page;
            refresh_current_view();
        } else {
            update_selection_visuals();
        }
    }

    void activate_selected() {
        if (selected_index < 0 || selected_index >= static_cast<int>(filtered_emojis.size())) return;
        const std::string& e = filtered_emojis[selected_index].glyph;
        launcher->copy_to_clipboard_and_quit(e);
    }

    void scroll_up() {
        if (current_page == 0) return;
        current_page--;
        refresh_current_view();
    }

    void scroll_down() {
        size_t total = filtered_emojis.size();
        if (total == 0) return;
        size_t max_page = (total - 1) / EMOJIS_PER_PAGE;
        if (current_page < max_page) {
            current_page++;
            refresh_current_view();
        }
    }

    void show_buttons() {
        for (auto* button : emoji_buttons) {
            gtk_widget_set_visible(button, TRUE);
        }
    }

    void hide_buttons() {
        for (auto* button : emoji_buttons) {
            gtk_widget_set_visible(button, FALSE);
        }
    }

    // Getters
    const std::vector<EmojiItem>& get_filtered_emojis() const { return filtered_emojis; }
    int get_selected_index() const { return selected_index; }
    size_t get_current_page() const { return current_page; }
    const std::vector<GtkWidget*>& get_emoji_buttons() const { return emoji_buttons; }
};