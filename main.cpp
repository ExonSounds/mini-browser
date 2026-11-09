// Minimal web browser: a search bar on top, the page view below.
// Built on GTK4 + WebKitGTK 6.0.
//
// By default pages are rendered from HTML and CSS only: JavaScript is off.
// The menu button at the top right holds toggles (all off by default) that
// turn extra features back on, the sites the current page loads scripts
// from, and a list of addresses whose scripts stay blocked even when
// scripts are allowed.

#include <gst/gst.h>
#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <webkit/webkit.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "vblank.h"

namespace {

// The HTML-only DuckDuckGo works without JavaScript.
const char *kHomePage = "https://html.duckduckgo.com/html/";
const char *kSearchUrl = "https://html.duckduckgo.com/html/?q=";
const char *kAppDir = "mini-browser";
const char *kBlocklistFile = "blocked-script-domains";

struct Browser {
    GtkWidget *entry = nullptr;
    WebKitWebView *view = nullptr;

    bool scripts_on = false;
    bool media_disabled = false;       // no audio output found at startup
    std::vector<std::string> blocked;  // normalized host names

    GtkWidget *popover = nullptr;
    GtkWidget *block_entry = nullptr;
    GtkWidget *block_list = nullptr;
    GtkWidget *page_list = nullptr;    // sites the current page loads scripts from
    GtkWidget *page_status = nullptr;  // shown when page_list is empty
    GtkWidget *page_note = nullptr;    // "scripts are off" hint

    WebKitUserContentFilterStore *filter_store = nullptr;
    guint filter_generation = 0;
    guint page_generation = 0;
    std::string last_uri;         // last real address shown
    bool page_committed = false;  // the new page has started to show
    std::string page_key;         // what page_list currently shows
    guint poll_id = 0;            // re-checks the list while a page loads
};

// ---------------------------------------------------------------- helpers --

// Remove every row but keep the placeholder (gtk_list_box_remove_all would
// destroy that too).
void clear_list(GtkWidget *list) {
    while (GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0)) {
        gtk_list_box_remove(GTK_LIST_BOX(list), GTK_WIDGET(row));
    }
}

// Treat the input as a URL if it has a scheme, or looks like a host name
// (contains a dot and no spaces). Anything else becomes a search query.
std::string to_url(const std::string &input) {
    if (input.find("://") != std::string::npos ||
        input.rfind("about:", 0) == 0) {
        return input;
    }
    if (input.find(' ') == std::string::npos &&
        input.find('.') != std::string::npos) {
        return "https://" + input;
    }
    char *escaped = g_uri_escape_string(input.c_str(), nullptr, FALSE);
    std::string url = std::string(kSearchUrl) + escaped;
    g_free(escaped);
    return url;
}

// Turn what the user typed ("https://cdn.Example.com:443/x.js", "*.ads.net")
// into a bare lowercase host name. Returns "" if it isn't a usable host.
std::string normalize_domain(std::string s) {
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    if (auto p = s.find("://"); p != std::string::npos) s.erase(0, p + 3);
    if (auto p = s.find_first_of("/?#"); p != std::string::npos) s.erase(p);
    if (auto p = s.find('@'); p != std::string::npos) s.erase(0, p + 1);
    if (auto p = s.find(':'); p != std::string::npos) s.erase(p);
    if (s.rfind("*.", 0) == 0) s.erase(0, 2);
    while (!s.empty() && s.front() == '.') s.erase(0, 1);
    while (!s.empty() && s.back() == '.') s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (s.empty()) return "";
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-')
            return "";
    }
    if (s.find("..") != std::string::npos) return "";
    return s;
}

std::string host_of(const char *uri) {
    if (!uri) return "";
    GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, nullptr);
    if (!parsed) return "";
    const char *host = g_uri_get_host(parsed);
    std::string result = host ? normalize_domain(host) : "";
    g_uri_unref(parsed);
    return result;
}

// "cdn.static.example.co.uk" -> "example.co.uk", using the public suffix
// list. IP addresses and single-label names come back unchanged.
std::string base_domain(const std::string &host) {
    const char *base = soup_tld_get_base_domain(host.c_str(), nullptr);
    return base ? base : host;
}

// WebKit plays sound through GStreamer's "autoaudiosink" (in the
// gst-plugins-good package). If it's missing, the page process aborts as
// soon as a page creates an <audio>/<video> element or uses Web Audio.
bool audio_output_available() {
    if (!gst_init_check(nullptr, nullptr, nullptr)) return false;
    GstElementFactory *sink = gst_element_factory_find("autoaudiosink");
    if (!sink) return false;
    gst_object_unref(sink);
    return true;
}

// A blocked entry covers the host itself and all its subdomains.
bool host_is_blocked(const Browser *b, const std::string &host) {
    if (host.empty()) return false;
    for (const auto &d : b->blocked) {
        if (host == d) return true;
        if (host.size() > d.size() &&
            host.compare(host.size() - d.size(), d.size(), d) == 0 &&
            host[host.size() - d.size() - 1] == '.') {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------ persistence --

std::string blocklist_path() {
    char *path = g_build_filename(g_get_user_config_dir(), kAppDir,
                                  kBlocklistFile, nullptr);
    std::string result = path;
    g_free(path);
    return result;
}

void load_blocklist(Browser *b) {
    char *contents = nullptr;
    if (!g_file_get_contents(blocklist_path().c_str(), &contents, nullptr,
                             nullptr)) {
        return;
    }
    char **lines = g_strsplit(contents, "\n", -1);
    for (char **line = lines; *line; ++line) {
        std::string d = normalize_domain(*line);
        if (!d.empty() &&
            std::find(b->blocked.begin(), b->blocked.end(), d) == b->blocked.end()) {
            b->blocked.push_back(d);
        }
    }
    g_strfreev(lines);
    g_free(contents);
}

void save_blocklist(const Browser *b) {
    std::string path = blocklist_path();
    char *dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    std::string contents;
    for (const auto &d : b->blocked) contents += d + "\n";
    GError *error = nullptr;
    if (!g_file_set_contents(path.c_str(), contents.c_str(), -1, &error)) {
        g_warning("Could not save blocklist: %s", error->message);
        g_error_free(error);
    }
}

// ------------------------------------------------------- script settings --

// Scripts on/off for the page about to be shown. Turning off
// "javascript-markup" too means <script> tags, on* handler attributes and
// javascript: links are dropped while parsing.
bool page_scripts_allowed(const Browser *b, const char *page_uri) {
    return b->scripts_on && !host_is_blocked(b, host_of(page_uri));
}

void apply_script_settings(Browser *b, const char *page_uri) {
    bool enabled = page_scripts_allowed(b, page_uri);
    WebKitSettings *settings = webkit_web_view_get_settings(b->view);
    webkit_settings_set_enable_javascript(settings, enabled);
    webkit_settings_set_enable_javascript_markup(settings, enabled);
}

void reload_current(Browser *b) {
    const char *uri = webkit_web_view_get_uri(b->view);
    if (!uri) return;
    apply_script_settings(b, uri);
    // reload() repeats the page already on screen; while a new page is still
    // loading there may be none, so restart that load instead.
    if (webkit_web_view_is_loading(b->view)) {
        std::string target = uri;
        webkit_web_view_load_uri(b->view, target.c_str());
    } else {
        webkit_web_view_reload(b->view);
    }
}

// The page's own address decides whether its inline scripts may run. This
// runs once the final address is known (after redirects), before the page
// is shown.
gboolean on_decide_policy(WebKitWebView *, WebKitPolicyDecision *decision,
                          WebKitPolicyDecisionType type, gpointer data) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE) return FALSE;
    auto *rd = WEBKIT_RESPONSE_POLICY_DECISION(decision);
    if (!webkit_response_policy_decision_is_main_frame_main_resource(rd)) {
        return FALSE;
    }
    WebKitURIResponse *response = webkit_response_policy_decision_get_response(rd);
    apply_script_settings(static_cast<Browser *>(data),
                          webkit_uri_response_get_uri(response));
    return FALSE;  // let WebKit carry on as normal
}

// Script files from blocked hosts (on any page, in any frame) are stopped
// by a WebKit content-blocker rule list.
std::string build_filter_json(const Browser *b) {
    std::string json = "[";
    for (size_t i = 0; i < b->blocked.size(); ++i) {
        std::string escaped;
        for (char c : b->blocked[i]) {
            if (c == '.') escaped += "\\\\.";  // regex "\." written as JSON
            else escaped += c;
        }
        if (i) json += ",";
        json += R"({"trigger":{"url-filter":"^[a-z]+://([^/]*\\.)?)" + escaped +
                R"([:/]","resource-type":["script"]},"action":{"type":"block"}})";
    }
    return json + "]";
}

struct FilterJob {
    Browser *browser;
    guint generation;
};

void on_filter_saved(GObject *source, GAsyncResult *result, gpointer data) {
    auto *job = static_cast<FilterJob *>(data);
    Browser *b = job->browser;
    bool stale = job->generation != b->filter_generation;
    delete job;

    GError *error = nullptr;
    WebKitUserContentFilter *filter = webkit_user_content_filter_store_save_finish(
        WEBKIT_USER_CONTENT_FILTER_STORE(source), result, &error);
    if (!filter) {
        g_warning("Could not compile script blocklist: %s", error->message);
        g_error_free(error);
        return;
    }
    if (!stale) {
        WebKitUserContentManager *manager =
            webkit_web_view_get_user_content_manager(b->view);
        webkit_user_content_manager_remove_all_filters(manager);
        webkit_user_content_manager_add_filter(manager, filter);
        if (b->scripts_on) reload_current(b);
    }
    webkit_user_content_filter_unref(filter);
}

void update_filter(Browser *b) {
    ++b->filter_generation;
    if (b->blocked.empty()) {
        webkit_user_content_manager_remove_all_filters(
            webkit_web_view_get_user_content_manager(b->view));
        if (b->scripts_on) reload_current(b);
        return;
    }
    std::string json = build_filter_json(b);
    GBytes *bytes = g_bytes_new(json.data(), json.size());
    webkit_user_content_filter_store_save(
        b->filter_store, "blocked-scripts", bytes, nullptr, on_filter_saved,
        new FilterJob{b, b->filter_generation});
    g_bytes_unref(bytes);
}

// ------------------------------------------------------------------- UI ----

void refresh_page_scripts(Browser *b);

void on_entry_activate(GtkEntry *entry, gpointer data) {
    auto *browser = static_cast<Browser *>(data);
    std::string text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (text.empty()) return;
    webkit_web_view_load_uri(browser->view, to_url(text).c_str());
    gtk_widget_grab_focus(GTK_WIDGET(browser->view));
}

// Keep the bar in sync with the page actually shown (links, redirects).
void on_uri_changed(WebKitWebView *view, GParamSpec *, gpointer data) {
    auto *browser = static_cast<Browser *>(data);
    const char *uri = webkit_web_view_get_uri(view);
    if (!uri || !*uri) return;  // e.g. after a crash; keep the last address
    browser->last_uri = uri;
    if (gtk_widget_has_focus(browser->entry)) return;
    gtk_editable_set_text(GTK_EDITABLE(browser->entry), uri);
}

// The page process died (a renderer or graphics-driver crash, or out of
// memory). Show a short explanation instead of a blank view.
void on_web_process_terminated(WebKitWebView *view,
                               WebKitWebProcessTerminationReason reason,
                               gpointer data) {
    auto *b = static_cast<Browser *>(data);
    b->page_committed = true;
    const char *why = reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT
                          ? "It used too much memory."
                          : "If this keeps happening, try switching hardware "
                            "acceleration on or off in the menu.";
    std::string html =
        "<!doctype html><title>Page crashed</title>"
        "<body style='font-family:sans-serif;margin:3em;line-height:1.5'>"
        "<h2>This page crashed</h2><p>" + std::string(why) +
        "</p><p>Press Enter in the address bar to try again.</p>";
    webkit_web_view_load_alternate_html(
        view, html.c_str(), b->last_uri.empty() ? nullptr : b->last_uri.c_str(),
        nullptr);
}

void set_gpu_enabled(Browser *b, bool enabled) {
    webkit_settings_set_hardware_acceleration_policy(
        webkit_web_view_get_settings(b->view),
        enabled ? WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS
                : WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
}

// Render at the monitor's full refresh rate. WebKit otherwise prefers ~60 fps
// even on faster displays.
void use_full_refresh_rate(WebKitSettings *settings) {
    WebKitFeatureList *features = webkit_settings_get_all_features();
    for (gsize i = 0; i < webkit_feature_list_get_length(features); ++i) {
        WebKitFeature *f = webkit_feature_list_get(features, i);
        if (g_str_equal(webkit_feature_get_identifier(f),
                        "PreferPageRenderingUpdatesNear60FPS")) {
            webkit_settings_set_feature_enabled(settings, f, FALSE);
        }
    }
    webkit_feature_list_unref(features);
}

// After each window frame, pass GTK's (compositor-reported) presentation
// timing to the refresh pacer so WebKit's ticks line up with real refreshes.
void on_after_paint(GdkFrameClock *clock, gpointer) {
    gint64 interval = 0, presentation = 0;
    gdk_frame_clock_get_refresh_info(clock, gdk_frame_clock_get_frame_time(clock),
                                     &interval, &presentation);
    vblank_set_phase_hint(presentation);
}

void on_window_realize(GtkWidget *window, gpointer) {
    if (GdkFrameClock *clock = gtk_widget_get_frame_clock(window)) {
        g_signal_connect(clock, "after-paint", G_CALLBACK(on_after_paint), nullptr);
    }
}

void on_gpu_toggled(GtkSwitch *sw, GParamSpec *, gpointer data) {
    auto *b = static_cast<Browser *>(data);
    set_gpu_enabled(b, gtk_switch_get_active(sw));
    reload_current(b);
}

void on_title_changed(WebKitWebView *view, GParamSpec *, gpointer window) {
    const char *title = webkit_web_view_get_title(view);
    gtk_window_set_title(GTK_WINDOW(window),
                         title && *title ? title : "Browser");
}

void on_scripts_toggled(GtkSwitch *sw, GParamSpec *, gpointer data) {
    auto *b = static_cast<Browser *>(data);
    b->scripts_on = gtk_switch_get_active(sw);
    gtk_widget_set_visible(b->page_note, !b->scripts_on);
    reload_current(b);
    refresh_page_scripts(b);
}

void refresh_block_list(Browser *b);

void add_blocked_domain(Browser *b, const std::string &domain) {
    if (std::find(b->blocked.begin(), b->blocked.end(), domain) != b->blocked.end()) {
        return;
    }
    b->blocked.push_back(domain);
    save_blocklist(b);
    refresh_block_list(b);
    refresh_page_scripts(b);
    update_filter(b);
}

void on_remove_clicked(GtkButton *button, gpointer data) {
    auto *b = static_cast<Browser *>(data);
    std::string domain = static_cast<const char *>(
        g_object_get_data(G_OBJECT(button), "domain"));
    b->blocked.erase(std::remove(b->blocked.begin(), b->blocked.end(), domain),
                     b->blocked.end());
    save_blocklist(b);
    refresh_block_list(b);
    refresh_page_scripts(b);
    update_filter(b);
}

// ------------------------------------------- scripts used by this page ----

// Two sources are combined:
//  1. The page's raw HTML as downloaded. This works even with scripts off,
//     when WebKit strips <script> tags before they reach the page.
//  2. When scripts may run on the page, the live page itself, which also
//     knows about script files added later by other scripts.

bool starts_with_ci(const std::string &s, size_t pos, const char *word) {
    size_t n = std::strlen(word);
    return pos + n <= s.size() && g_ascii_strncasecmp(s.data() + pos, word, n) == 0;
}

size_t find_ci(const std::string &s, const char *word, size_t from) {
    for (size_t i = from; i < s.size(); ++i) {
        if (starts_with_ci(s, i, word)) return i;
    }
    return std::string::npos;
}

bool is_space(char c) { return g_ascii_isspace(c); }

// Absolute URLs of every <script src="..."> in the HTML.
std::vector<std::string> script_srcs_in_html(const std::string &html,
                                             const char *base_uri) {
    std::vector<std::string> urls;
    const size_t n = html.size();
    size_t i = 0;
    while ((i = html.find('<', i)) != std::string::npos) {
        if (html.compare(i, 4, "<!--") == 0) {  // skip comments
            size_t end = html.find("-->", i + 4);
            if (end == std::string::npos) break;
            i = end + 3;
            continue;
        }
        if (!starts_with_ci(html, i + 1, "script") || i + 7 >= n ||
            !(is_space(html[i + 7]) || html[i + 7] == '>' || html[i + 7] == '/')) {
            ++i;
            continue;
        }

        // Read the tag's attributes up to '>'.
        size_t p = i + 7;
        std::string src;
        while (p < n && html[p] != '>') {
            if (is_space(html[p]) || html[p] == '/') {
                ++p;
                continue;
            }
            size_t name_start = p;
            while (p < n && !is_space(html[p]) && html[p] != '=' &&
                   html[p] != '>' && html[p] != '/') {
                ++p;
            }
            std::string name = html.substr(name_start, p - name_start);
            while (p < n && is_space(html[p])) ++p;
            std::string value;
            if (p < n && html[p] == '=') {
                ++p;
                while (p < n && is_space(html[p])) ++p;
                if (p < n && (html[p] == '"' || html[p] == '\'')) {
                    char quote = html[p++];
                    size_t end = html.find(quote, p);
                    if (end == std::string::npos) end = n;
                    value = html.substr(p, end - p);
                    p = std::min(end + 1, n);
                } else {
                    size_t start = p;
                    while (p < n && !is_space(html[p]) && html[p] != '>') ++p;
                    value = html.substr(start, p - start);
                }
            }
            if (src.empty() && g_ascii_strcasecmp(name.c_str(), "src") == 0) {
                src = value;
            }
        }

        if (!src.empty()) {
            for (size_t amp; (amp = src.find("&amp;")) != std::string::npos;) {
                src.replace(amp, 5, "&");
            }
            src.erase(0, src.find_first_not_of(" \t\r\n"));
            src.erase(src.find_last_not_of(" \t\r\n") + 1);
            char *absolute = g_uri_resolve_relative(base_uri, src.c_str(),
                                                    G_URI_FLAGS_NONE, nullptr);
            if (absolute) {
                urls.push_back(absolute);
                g_free(absolute);
            }
        }

        // Skip the script's body so code inside it isn't read as tags.
        size_t close = find_ci(html, "</script", p);
        if (close == std::string::npos) break;
        i = close + 8;
    }
    return urls;
}

// Runs in a private script world, so the page can't see or tamper with it.
// <script src> elements are listed even if their load was blocked; the
// resource timings add script files the page pulled in after loading.
const char *kListScriptsJs = R"JS((() => {
  const urls = new Set();
  for (const s of document.querySelectorAll('script[src]')) urls.add(s.src);
  for (const e of performance.getEntriesByType('resource'))
    if (e.initiatorType === 'script') urls.add(e.name);
  return [...urls].join('\n');
})())JS";

void show_page_message(Browser *b, const char *text) {
    clear_list(b->page_list);
    gtk_label_set_text(GTK_LABEL(b->page_status), text);
    b->page_key.clear();
}

void on_page_block_clicked(GtkButton *button, gpointer data) {
    add_blocked_domain(static_cast<Browser *>(data),
                       static_cast<const char *>(
                           g_object_get_data(G_OBJECT(button), "domain")));
}

struct SiteScripts {
    int count = 0;
    bool all_blocked = true;
};

void add_page_row(Browser *b, const std::string &site, const SiteScripts &info,
                  bool this_site) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(row, 6);
    gtk_widget_set_margin_end(row, 6);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    std::string title = this_site ? site + " (this site)" : site;
    GtkWidget *name = gtk_label_new(title.c_str());
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    char *count = g_strdup_printf(info.count == 1 ? "%d script file"
                                                  : "%d script files",
                                  info.count);
    GtkWidget *detail = gtk_label_new(count);
    g_free(count);
    gtk_label_set_xalign(GTK_LABEL(detail), 0.0f);
    gtk_widget_add_css_class(detail, "dim-label");
    gtk_widget_add_css_class(detail, "caption");
    gtk_box_append(GTK_BOX(text), name);
    gtk_box_append(GTK_BOX(text), detail);
    gtk_box_append(GTK_BOX(row), text);

    if (info.all_blocked) {
        GtkWidget *state = gtk_label_new("Blocked");
        gtk_widget_add_css_class(state, "dim-label");
        gtk_widget_set_margin_top(state, 8);
        gtk_widget_set_margin_bottom(state, 8);
        gtk_box_append(GTK_BOX(row), state);
    } else {
        GtkWidget *block = gtk_button_new_with_label("Block");
        gtk_widget_set_valign(block, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(block, 4);
        gtk_widget_set_margin_bottom(block, 4);
        std::string tip = "Block scripts from " + site + " and its subdomains";
        if (this_site) tip += ". This turns off all scripts on this site";
        gtk_widget_set_tooltip_text(block, tip.c_str());
        g_object_set_data_full(G_OBJECT(block), "domain", g_strdup(site.c_str()),
                               g_free);
        g_signal_connect(block, "clicked", G_CALLBACK(on_page_block_clicked), b);
        gtk_box_append(GTK_BOX(row), block);
    }
    gtk_list_box_append(GTK_LIST_BOX(b->page_list), row);
}

struct PageScriptsJob {
    Browser *browser;
    guint generation;
    std::vector<std::string> urls;
};

void show_page_scripts(Browser *b, const std::vector<std::string> &urls) {
    std::map<std::string, SiteScripts> sites;
    for (const auto &url : std::set<std::string>(urls.begin(), urls.end())) {
        std::string host = host_of(url.c_str());
        if (host.empty()) continue;  // data:, blob: and the like
        SiteScripts &site = sites[base_domain(host)];
        ++site.count;
        if (!host_is_blocked(b, host)) site.all_blocked = false;
    }

    // Skip rebuilding when nothing changed, so a button doesn't get
    // replaced under the pointer while the list is re-checked.
    std::string own = base_domain(host_of(webkit_web_view_get_uri(b->view)));
    std::string key = "list:" + own;
    for (const auto &[site, info] : sites) {
        key += "|" + site + ":" + std::to_string(info.count) +
               (info.all_blocked ? "b" : "");
    }
    if (key == b->page_key) return;

    show_page_message(b, "This page doesn't load any script files.");
    b->page_key = key;
    if (auto it = sites.find(own); it != sites.end()) {
        add_page_row(b, it->first, it->second, true);
    }
    for (const auto &[site, info] : sites) {
        if (site != own) add_page_row(b, site, info, false);
    }
}

// Step 2: add what the live page reports, then show the combined list.
void on_page_scripts_result(GObject *source, GAsyncResult *result, gpointer data) {
    auto *job = static_cast<PageScriptsJob *>(data);
    Browser *b = job->browser;

    GError *error = nullptr;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(source), result, &error);
    if (value) {
        char *text = jsc_value_to_string(value);
        char **urls = g_strsplit(text, "\n", -1);
        for (char **url = urls; *url; ++url) {
            if (**url) job->urls.push_back(*url);
        }
        g_strfreev(urls);
        g_free(text);
        g_object_unref(value);
    } else {
        g_error_free(error);  // the HTML list is still worth showing
    }

    if (job->generation == b->page_generation) show_page_scripts(b, job->urls);
    delete job;
}

// Step 1: read the page's HTML as downloaded.
void on_page_html(GObject *source, GAsyncResult *result, gpointer data) {
    auto *job = static_cast<PageScriptsJob *>(data);
    Browser *b = job->browser;
    auto *resource = WEBKIT_WEB_RESOURCE(source);

    gsize length = 0;
    GError *error = nullptr;
    guchar *bytes = webkit_web_resource_get_data_finish(resource, result, &length,
                                                        &error);
    if (job->generation != b->page_generation) {
        g_free(bytes);
        g_clear_error(&error);
        delete job;
        return;
    }
    if (bytes) {
        std::string html(reinterpret_cast<char *>(bytes), length);
        g_free(bytes);
        job->urls = script_srcs_in_html(html, webkit_web_resource_get_uri(resource));
    } else {
        g_clear_error(&error);
    }

    if (page_scripts_allowed(b, webkit_web_view_get_uri(b->view))) {
        webkit_web_view_evaluate_javascript(b->view, kListScriptsJs, -1,
                                            "mini-browser", nullptr, nullptr,
                                            on_page_scripts_result, job);
    } else {
        show_page_scripts(b, job->urls);
        delete job;
    }
}

// Only does work while the menu is open.
void refresh_page_scripts(Browser *b) {
    ++b->page_generation;
    if (!b->popover || !gtk_widget_get_visible(b->popover)) return;
    gtk_widget_set_visible(b->page_note, !b->scripts_on);

    const char *uri = webkit_web_view_get_uri(b->view);
    WebKitWebResource *page = webkit_web_view_get_main_resource(b->view);
    if (!uri) {
        show_page_message(b, "No page loaded.");
    } else if (!b->page_committed) {
        show_page_message(b, "Loading…");
    } else if (!page) {
        show_page_message(b, "Couldn't read this page's scripts.");
    } else {
        webkit_web_resource_get_data(page, nullptr, on_page_html,
                                     new PageScriptsJob{b, b->page_generation, {}});
    }
}

void on_load_changed(WebKitWebView *, WebKitLoadEvent event, gpointer data) {
    auto *b = static_cast<Browser *>(data);
    if (event == WEBKIT_LOAD_REDIRECTED) return;
    // FINISHED also follows a failed load, so the view is showing something.
    b->page_committed = event != WEBKIT_LOAD_STARTED;
    refresh_page_scripts(b);
}

// Big pages keep pulling in scripts for a while after they first show, so
// re-check every couple of seconds while the menu is open and loading.
gboolean poll_page_scripts(gpointer data) {
    auto *b = static_cast<Browser *>(data);
    if (webkit_web_view_is_loading(b->view)) refresh_page_scripts(b);
    return G_SOURCE_CONTINUE;
}

void on_popover_show(GtkWidget *, gpointer data) {
    auto *b = static_cast<Browser *>(data);
    refresh_page_scripts(b);
    if (!b->poll_id) b->poll_id = g_timeout_add_seconds(2, poll_page_scripts, b);
}

void on_popover_hide(GtkWidget *, gpointer data) {
    auto *b = static_cast<Browser *>(data);
    if (b->poll_id) {
        g_source_remove(b->poll_id);
        b->poll_id = 0;
    }
}

void refresh_block_list(Browser *b) {
    clear_list(b->block_list);
    for (const auto &d : b->blocked) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(row, 6);

        GtkWidget *label = gtk_label_new(d.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_hexpand(label, TRUE);

        GtkWidget *remove = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_widget_add_css_class(remove, "flat");
        gtk_widget_set_tooltip_text(remove, "Stop blocking this address");
        g_object_set_data_full(G_OBJECT(remove), "domain", g_strdup(d.c_str()),
                               g_free);
        g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_clicked), b);

        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(GTK_BOX(row), remove);
        gtk_list_box_append(GTK_LIST_BOX(b->block_list), row);
    }
}

void on_add_blocked(GtkWidget *, gpointer data) {
    auto *b = static_cast<Browser *>(data);
    std::string domain =
        normalize_domain(gtk_editable_get_text(GTK_EDITABLE(b->block_entry)));
    if (domain.empty()) {
        gtk_widget_add_css_class(b->block_entry, "error");
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(b->block_entry), "");
    add_blocked_domain(b, domain);
}

void on_block_entry_changed(GtkEditable *editable, gpointer) {
    gtk_widget_remove_css_class(GTK_WIDGET(editable), "error");
}

// One "label ........ [switch]" row in the menu. Switches start off.
void add_toggle(GtkWidget *list, const char *label, GCallback on_toggled,
                Browser *browser) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *text = gtk_label_new(label);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);

    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), FALSE);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    g_signal_connect(sw, "notify::active", on_toggled, browser);

    gtk_box_append(GTK_BOX(row), text);
    gtk_box_append(GTK_BOX(row), sw);
    gtk_box_append(GTK_BOX(list), row);
}

// A list box inside a scroller that grows up to max_height, then scrolls.
GtkWidget *scrolling_list(GtkWidget *list_box, int max_height) {
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list_box, "boxed-list");
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), max_height);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list_box);
    return scroller;
}

GtkWidget *placeholder_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_set_margin_top(label, 6);
    gtk_widget_set_margin_bottom(label, 6);
    gtk_widget_set_margin_start(label, 6);
    gtk_widget_set_margin_end(label, 6);
    return label;
}

GtkWidget *build_block_section(Browser *b) {
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *page_heading = gtk_label_new("This page loads scripts from:");
    gtk_label_set_xalign(GTK_LABEL(page_heading), 0.0f);
    gtk_box_append(GTK_BOX(section), page_heading);

    b->page_note = gtk_label_new(
        "Scripts are off, so none of these run now. Anything you block here "
        "stays blocked when you turn scripts on.");
    gtk_label_set_wrap(GTK_LABEL(b->page_note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(b->page_note), 0.0f);
    gtk_label_set_max_width_chars(GTK_LABEL(b->page_note), 40);
    gtk_widget_add_css_class(b->page_note, "dim-label");
    gtk_widget_add_css_class(b->page_note, "caption");
    gtk_widget_set_visible(b->page_note, !b->scripts_on);
    gtk_box_append(GTK_BOX(section), b->page_note);

    b->page_list = gtk_list_box_new();
    b->page_status = placeholder_label("");
    gtk_list_box_set_placeholder(GTK_LIST_BOX(b->page_list), b->page_status);
    gtk_box_append(GTK_BOX(section), scrolling_list(b->page_list, 220));

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(separator, 4);
    gtk_widget_set_margin_bottom(separator, 4);
    gtk_box_append(GTK_BOX(section), separator);

    GtkWidget *heading = gtk_label_new("Block scripts from these addresses:");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_box_append(GTK_BOX(section), heading);

    GtkWidget *add_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    b->block_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(b->block_entry), "e.g. tracker.com");
    gtk_widget_set_hexpand(b->block_entry, TRUE);
    g_signal_connect(b->block_entry, "activate", G_CALLBACK(on_add_blocked), b);
    g_signal_connect(b->block_entry, "changed",
                     G_CALLBACK(on_block_entry_changed), nullptr);

    GtkWidget *add = gtk_button_new_with_label("Block");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_blocked), b);

    gtk_box_append(GTK_BOX(add_row), b->block_entry);
    gtk_box_append(GTK_BOX(add_row), add);
    gtk_box_append(GTK_BOX(section), add_row);

    b->block_list = gtk_list_box_new();
    gtk_list_box_set_placeholder(GTK_LIST_BOX(b->block_list),
                                 placeholder_label("No blocked addresses"));
    gtk_box_append(GTK_BOX(section), scrolling_list(b->block_list, 200));

    refresh_block_list(b);
    return section;
}

GtkWidget *build_menu_button(Browser *b) {
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(list, 6);
    gtk_widget_set_margin_end(list, 6);
    gtk_widget_set_margin_top(list, 6);
    gtk_widget_set_margin_bottom(list, 6);
    gtk_widget_set_size_request(list, 320, -1);

    add_toggle(list, "Allow scripts (JavaScript)",
               G_CALLBACK(on_scripts_toggled), b);
    gtk_box_append(GTK_BOX(list), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(list), build_block_section(b));
    gtk_box_append(GTK_BOX(list), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    // Off by default: GPU rendering crashes on some driver setups.
    add_toggle(list, "Hardware acceleration (GPU)", G_CALLBACK(on_gpu_toggled), b);
    if (b->media_disabled) {
        GtkWidget *note = gtk_label_new(
            "Audio and video are off: GStreamer has no audio output. Install "
            "gst-plugins-good and restart the browser to turn them on.");
        gtk_label_set_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
        gtk_label_set_max_width_chars(GTK_LABEL(note), 40);
        gtk_widget_add_css_class(note, "dim-label");
        gtk_widget_add_css_class(note, "caption");
        gtk_box_append(GTK_BOX(list), note);
    }

    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(popover), list);
    b->popover = popover;
    g_signal_connect(popover, "show", G_CALLBACK(on_popover_show), b);
    g_signal_connect(popover, "hide", G_CALLBACK(on_popover_hide), b);
    // Line up the popover's right edge with the button so it opens leftward
    // and stays inside the window (some compositors don't shift it back).
    gtk_widget_set_halign(popover, GTK_ALIGN_END);

    GtkWidget *button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(button, "Settings");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
    return button;
}

void on_activate(GtkApplication *app, gpointer) {
    auto *b = new Browser{};

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Browser");
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 800);
    g_object_set_data_full(G_OBJECT(window), "browser", b, [](gpointer p) {
        auto *browser = static_cast<Browser *>(p);
        if (browser->poll_id) g_source_remove(browser->poll_id);
        g_clear_object(&browser->filter_store);
        delete browser;
    });

    char *store_path = g_build_filename(g_get_user_cache_dir(), kAppDir,
                                        "content-filters", nullptr);
    b->filter_store = webkit_user_content_filter_store_new(store_path);
    g_free(store_path);

    b->view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_widget_set_vexpand(GTK_WIDGET(b->view), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(b->view), TRUE);
    apply_script_settings(b, nullptr);  // scripts start off
    set_gpu_enabled(b, false);          // so does GPU rendering
    use_full_refresh_rate(webkit_web_view_get_settings(b->view));
    if (!audio_output_available()) {
        // Without an audio output, media would crash the page; turn it off.
        b->media_disabled = true;
        WebKitSettings *settings = webkit_web_view_get_settings(b->view);
        webkit_settings_set_enable_media(settings, FALSE);
        webkit_settings_set_enable_webaudio(settings, FALSE);
        g_printerr("mini-browser: no GStreamer audio output (autoaudiosink), so "
                   "audio and video are off. Install gst-plugins-good.\n");
    }

    load_blocklist(b);
    update_filter(b);

    // Top bar: search entry filling the width, menu button on the right.
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);

    b->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(b->entry), "Search or enter address");
    gtk_widget_set_hexpand(b->entry, TRUE);

    gtk_box_append(GTK_BOX(bar), b->entry);
    gtk_box_append(GTK_BOX(bar), build_menu_button(b));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(b->view));
    gtk_window_set_child(GTK_WINDOW(window), box);

    g_signal_connect(window, "realize", G_CALLBACK(on_window_realize), nullptr);
    g_signal_connect(b->entry, "activate", G_CALLBACK(on_entry_activate), b);
    g_signal_connect(b->view, "notify::uri", G_CALLBACK(on_uri_changed), b);
    g_signal_connect(b->view, "notify::title", G_CALLBACK(on_title_changed), window);
    g_signal_connect(b->view, "decide-policy", G_CALLBACK(on_decide_policy), b);
    g_signal_connect(b->view, "load-changed", G_CALLBACK(on_load_changed), b);
    g_signal_connect(b->view, "web-process-terminated",
                     G_CALLBACK(on_web_process_terminated), b);

    webkit_web_view_load_uri(b->view, kHomePage);
    gtk_window_present(GTK_WINDOW(window));
}

}  // namespace

int main(int argc, char **argv) {
    // GTK's GL renderer keeps WebKit at the full refresh rate. With the
    // default Vulkan renderer each frame takes an extra refresh to hand back,
    // which halves the frame rate while scrolling (90 instead of 180 fps on a
    // 180 Hz screen). A GSK_RENDERER set by the user still wins.
    g_setenv("GSK_RENDERER", "gl", FALSE);

    GtkApplication *app =
        gtk_application_new("org.example.MiniBrowser", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
