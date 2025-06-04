#include <ncurses.h>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <ctime>
#include <cctype>
#include <thread>
#include <atomic>
#include <mutex>
#include <iostream>
#include <fstream>
#include <string>
#include <ctime>
#include <mutex>

std::mutex logMutex;

enum class LogLevel {
    INFO,
    WARNING,
    ERROR,
    DEBUG
};

using namespace std;

// 关键词集合
const set<string> cpp_keywords = {
    "int","for","if","else","while","return","switch","case","break","const","void","class",
    "public","private","protected","struct","new","delete","virtual","override","static","using",
    "namespace","include","this","template","typename","auto","long","short","unsigned","signed",
    "operator","try","catch","throw"
};
const set<string> py_keywords = {
    "def","if","else","elif","for","while","return","import","from","class","try","except",
    "finally","with","as","lambda","pass","break","continue","yield","in","is","not","and","or",
    "print","self","global","nonlocal","assert","del","raise"
};
const set<string> js_keywords = {
    "function","var","let","const","if","else","for","while","return","switch","case","break",
    "class","constructor","new","import","export","extends","from","try","catch","finally","throw"
};
const set<string> java_keywords = {
    "int","public","private","protected","void","class","static","final","return","if","else","for",
    "while","switch","case","break","new","import","package","extends","implements","try","catch",
    "finally","this","super"
};
const set<string> json_keywords = {"true","false","null"};

const int CACHE_SIZE = 100;  // 缓存窗口大小，可调整

struct EditorState {
    vector<string> cache_lines;   // 缓存当前窗口附近的若干行
int file_rowoff = 0;         // 当前缓存内容在文件中的起始行号
int total_lines = 0;         // 文件总行数
vector<bool> dirty_flags;    // 缓存区每行是否被修改
    string filename, statusmsg;
    std::mutex file_mutex;
    std::atomic<bool> loading{false};
    std::atomic<bool> stop_loading{false};
    int loading_target_row = 0;
    int cx = 0, cy = 0;
    int rowoff = 0;
    bool dirty = false;
    bool newfile = false;
    // 搜索相关
    string search_word = "";
    vector<pair<int, int>> search_results;
    int search_idx = 0;
    bool search_flash = false;
    clock_t last_search_time = 0;
};

bool is_code_file(const string& filename) {
    size_t pos = filename.find_last_of('.');
    if (pos == string::npos) return false;
    string ext = filename.substr(pos + 1);
    vector<string> code_ext = {"cpp", "py", "js", "json", "java"};
    return find(code_ext.begin(), code_ext.end(), ext) != code_ext.end();
}

string get_ext(const string& filename) {
    size_t pos = filename.find_last_of('.');
    if (pos == string::npos) return "";
    return filename.substr(pos + 1);
}

void draw_code_row(const string& line, int y, const string& ext, EditorState &ed, int filerow) {
    int x = 0;
    const set<string>* keywords = nullptr;
    if (ext == "cpp") keywords = &cpp_keywords;
    else if (ext == "py") keywords = &py_keywords;
    else if (ext == "js") keywords = &js_keywords;
    else if (ext == "java") keywords = &java_keywords;
    else if (ext == "json") keywords = &json_keywords;

    int highlight_start = -1, highlight_len = 0;
    if (ed.search_flash && !ed.search_word.empty()) {
        if (!ed.search_results.empty() && ed.search_idx < (int)ed.search_results.size()) {
            int sy = ed.search_results[ed.search_idx].first;
            int sx = ed.search_results[ed.search_idx].second;
            if (sy == filerow) {
                highlight_start = sx;
                highlight_len = ed.search_word.size();
            }
        }
    }

    for (size_t i = 0; i < line.size();) {
        // 搜索高亮
        if (highlight_start == x) {
            attron(COLOR_PAIR(5) | A_STANDOUT);
            for (int k = 0; k < highlight_len && i < line.size(); ++k, ++i, ++x)
                mvaddch(y, x, line[i]);
            attroff(COLOR_PAIR(5) | A_STANDOUT);
            continue;
        }
        // 注释高亮
        if ((ext == "cpp" || ext == "java" || ext == "js") && line[i] == '/' && i+1 < line.size() && line[i+1] == '/') {
            attron(COLOR_PAIR(3));
            mvprintw(y, x, "%s", line.substr(i).c_str());
            attroff(COLOR_PAIR(3));
            break;
        }
        if (ext == "py" && line[i] == '#') {
            attron(COLOR_PAIR(3));
            mvprintw(y, x, "%s", line.substr(i).c_str());
            attroff(COLOR_PAIR(3));
            break;
        }
        // 字符串高亮
        if (line[i] == '"' || line[i] == '\'') {
            int quote = line[i];
            attron(COLOR_PAIR(2));
            mvaddch(y, x++, line[i++]);
            while (i < line.size()) {
                mvaddch(y, x++, line[i]);
                if (line[i++] == quote) break;
            }
            attroff(COLOR_PAIR(2));
            continue;
        }
        // 关键字高亮
        if (keywords && (isalpha(line[i]) || line[i] == '_')) {
            string word;
            size_t start = i;
            while (i < line.size() && (isalnum(line[i]) || line[i] == '_')) word += line[i++];
            if (keywords->count(word)) {
                attron(COLOR_PAIR(1));
                mvprintw(y, x, "%s", word.c_str());
                attroff(COLOR_PAIR(1));
            } else {
                mvprintw(y, x, "%s", word.c_str());
            }
            x += word.size();
            continue;
        }
        // 数字高亮
        if (isdigit(line[i])) {
            size_t start = i;
            while (i < line.size() && isdigit(line[i])) ++i;
            attron(COLOR_PAIR(4));
            mvprintw(y, x, "%s", line.substr(start, i-start).c_str());
            attroff(COLOR_PAIR(4));
            x += i - start;
            continue;
        }
        // 其它
        mvaddch(y, x++, line[i++]);
    }
}

void WriteLog(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> guard(logMutex);
    std::ofstream logfile("file.log", std::ios::app);
    if (logfile.is_open()) {
        std::time_t now = std::time(nullptr);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::string levelStr;
        switch (level) {
            case LogLevel::INFO: levelStr = "INFO"; break;
            case LogLevel::WARNING: levelStr = "WARNING"; break;
            case LogLevel::ERROR: levelStr = "ERROR"; break;
            case LogLevel::DEBUG: levelStr = "DEBUG"; break;
        }
        logfile << "[" << buf << "][" << levelStr << "] " << message << std::endl;
    }
}

void async_load_cache(EditorState& ed, int target_row) {
    // 防止多重加载
    if (ed.loading) return;
    ed.loading = true;
    ed.stop_loading = false;
    ed.loading_target_row = target_row;

    WriteLog(LogLevel::DEBUG, "Begin async_load_cache at row " + std::to_string(target_row) + " for file: " + ed.filename);

    std::thread([&ed, target_row]() {
        std::ifstream fin(ed.filename);
        if (!fin) {
            WriteLog(LogLevel::ERROR, "async_load_cache failed to open file: " + ed.filename);
            ed.loading = false;
            return;
        }
        std::string s;
        int cur = 0;
        int start = std::max(target_row - CACHE_SIZE/2, 0);
        std::vector<std::string> new_lines;
        std::vector<bool> new_dirty;
        while (std::getline(fin, s)) {
            if (cur >= start && (int)new_lines.size() < CACHE_SIZE) {
                new_lines.push_back(s);
                new_dirty.push_back(false);
            }
            cur++;
            if ((int)new_lines.size() >= CACHE_SIZE) break;
            if (ed.stop_loading) {
                WriteLog(LogLevel::WARNING, "async_load_cache interrupted for file: " + ed.filename);
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lk(ed.file_mutex);
            ed.cache_lines = new_lines;
            ed.dirty_flags = new_dirty;
            ed.file_rowoff = start;
        }
        WriteLog(LogLevel::DEBUG, "async_load_cache finished for file: " + ed.filename + ", loaded lines: " + std::to_string(new_lines.size()));
        ed.loading = false;
    }).detach();
}

void ensure_cache(EditorState& ed, int target_row) {
    if (target_row < ed.file_rowoff || target_row >= ed.file_rowoff + (int)ed.cache_lines.size()) {
        // 需要重新加载缓存
        ifstream fin(ed.filename);
        string s;
        int cur = 0;
        int start = max(target_row - CACHE_SIZE/2, 0);
        ed.cache_lines.clear();
        ed.dirty_flags.clear();
        while (getline(fin, s)) {
            if (cur >= start && (int)ed.cache_lines.size() < CACHE_SIZE) {
                ed.cache_lines.push_back(s);
                ed.dirty_flags.push_back(false);
            }
            cur++;
            if ((int)ed.cache_lines.size() >= CACHE_SIZE) break;
        }
        ed.file_rowoff = start;
    }
}

void draw_rows(EditorState &ed, int rows, int cols) {
    std::lock_guard<std::mutex> lk(ed.file_mutex);
    string ext = get_ext(ed.filename);
    bool color = is_code_file(ed.filename);
    for (int y = 0; y < rows-3; ++y) {
        int filerow = y + ed.rowoff;
        move(y, 0);
        clrtoeol();
        if (filerow < (int)ed.cache_lines.size()) {
            if (color)
                draw_code_row(ed.cache_lines[filerow], y, ext, ed, filerow);
            else {
                if (!ed.search_results.empty() && ed.search_idx < (int)ed.search_results.size()) {
                    int sy = ed.search_results[ed.search_idx].first;
                    int sx = ed.search_results[ed.search_idx].second;
                    if (sy == filerow) {
                        mvprintw(y, 0, "%.*s", sx, ed.cache_lines[filerow].c_str());
                        attron(COLOR_PAIR(5) | A_STANDOUT);
                        printw("%.*s", (int)ed.search_word.size(), ed.cache_lines[filerow].c_str() + sx);
                        attroff(COLOR_PAIR(5) | A_STANDOUT);
                        printw("%s", ed.cache_lines[filerow].c_str() + sx + ed.search_word.size());
                        continue;
                    }
                }
                mvprintw(y, 0, "%s", ed.cache_lines[filerow].c_str());
            }
        }
    }
}

void draw_status(EditorState &ed, int rows, int cols) {
    attron(A_REVERSE);
    string stat = " " + ed.filename;
    if (ed.newfile) stat += " (new file)";
    if (ed.dirty) stat += " *";
    mvprintw(rows-3, 0, "%-*s", cols, stat.c_str());
    attroff(A_REVERSE);
}

void draw_msg(EditorState &ed, int rows, int cols) {
    move(rows-2, 0);
    clrtoeol();
    mvprintw(rows-2, 0, "%-*s", cols, ed.statusmsg.c_str());
}

void draw_shortcuts(int rows, int cols) {
    attron(A_REVERSE);
    mvprintw(rows-1, 0, "^O Save  ^X Exit  ^C Cancel  ^F Find  ^G Help");
    attroff(A_REVERSE);
}

void set_status(EditorState &ed, const string &msg) {
    ed.statusmsg = msg;
}

void open_file(EditorState &ed, const string &fname) {
    ed.filename = fname;
    ed.cache_lines.clear();
    ed.dirty_flags.clear();
    ifstream fin(fname);
    ed.file_rowoff = 0;
    ed.total_lines = 0;

    if (!fin) {
        WriteLog(LogLevel::INFO, "Try open file (new): " + fname);
        ed.cache_lines.push_back("");
        ed.dirty_flags.push_back(false);
        ed.newfile = true;
        set_status(ed, fname + " (new file) ");
        ed.total_lines = 1;
        WriteLog(LogLevel::WARNING, "File not found, treat as new file: " + fname);
    } else {
        WriteLog(LogLevel::INFO, "Open file: " + fname + " success");
        string s;
        int cnt = 0;
        while (getline(fin, s)) {
            ed.cache_lines.push_back(s);
            ed.dirty_flags.push_back(false);
            cnt++;
        }
        ed.newfile = false;
        set_status(ed, fname);
        ed.total_lines = cnt;
        WriteLog(LogLevel::DEBUG, "File loaded: " + fname + ", lines=" + to_string(cnt));
    }

    ed.dirty = false;
    ed.cx = ed.cy = ed.rowoff = 0;
    WriteLog(LogLevel::INFO, "open_file finished: " + fname);
}

void save_file(EditorState &ed, const string &fname) {
    // 1. 先加载全文件内容
    vector<string> all_lines;
    ifstream fin(fname);
    string s;
    while (getline(fin, s)) all_lines.push_back(s);
    fin.close();

    // 2. 用缓存区内容覆盖对应片段
    int start = ed.file_rowoff;
    for (int i = 0; i < (int)ed.cache_lines.size(); ++i) {
        if (start + i < (int)all_lines.size()) {
            if (ed.dirty_flags[i])  // 只覆盖被编辑过的行
                all_lines[start + i] = ed.cache_lines[i];
        } else {
            // 缓存区超出原文件长度，则追加
            all_lines.push_back(ed.cache_lines[i]);
        }
    }

    // 3. 写回文件
    ofstream fout(fname);
    for (const auto &line : all_lines) fout << line << "\n";
    fout.close();

    ed.filename = fname;
    ed.newfile = false;
    ed.dirty = false;
    ed.dirty_flags.assign(ed.cache_lines.size(), false); // 保存后清除脏标记
    set_status(ed, "Wrote " + to_string(all_lines.size()) + " lines");
}

void editor_move_cursor(EditorState &ed, int key, int rows, int cols) {
    int screen_rows = rows - 3;

    // 计算实际文件行号
    int actual_row = ed.file_rowoff + ed.cy;

    switch (key) {
    case KEY_UP:
        if (ed.cy > 0) {
            ed.cy--;
        } else if (actual_row > 0) {
            // 到达缓存区顶端，向上移动片段
            if (!ed.loading) {
    async_load_cache(ed, actual_row - 1);
}
            ed.cy = 0;
        }
        break;
    case KEY_DOWN:
        if (ed.cy < (int)ed.cache_lines.size() - 1 && actual_row + 1 < ed.total_lines) {
            ed.cy++;
        } else if (actual_row + 1 < ed.total_lines) {
            // 到达缓存区底端，向下移动片段
            if (!ed.loading) {
    async_load_cache(ed, actual_row + 1);
}
            ed.cy = min((int)ed.cache_lines.size() - 1, ed.cy);
        }
        break;
    case KEY_LEFT:
        if (ed.cx > 0) {
            ed.cx--;
        } else if (ed.cy > 0) {
            ed.cy--;
            ed.cx = ed.cache_lines[ed.cy].size();
        } else if (actual_row > 0) {
            // 顶行再左，加载上一片段
            if (!ed.loading) {
    async_load_cache(ed, actual_row - 1);
}
            ed.cy = 0;
            ed.cx = ed.cache_lines[ed.cy].size();
        }
        break;
    case KEY_RIGHT:
        if (ed.cx < (int)ed.cache_lines[ed.cy].size()) {
            ed.cx++;
        } else if (ed.cy < (int)ed.cache_lines.size() - 1 && actual_row + 1 < ed.total_lines) {
            ed.cy++;
            ed.cx = 0;
        } else if (actual_row + 1 < ed.total_lines) {
            // 底行再右，加载下一片段
            if (!ed.loading) {
    async_load_cache(ed, actual_row + 1);
}
            ed.cy = min((int)ed.cache_lines.size() - 1, ed.cy + 1);
            ed.cx = 0;
        }
        break;
    }

    // 保证 cx 不越界
    ed.cx = min(ed.cx, (int)ed.cache_lines[ed.cy].size());

    // 屏幕滚动行偏移
    if (ed.cy < ed.rowoff) ed.rowoff = ed.cy;
    if (ed.cy >= ed.rowoff + screen_rows) ed.rowoff = ed.cy - (screen_rows - 1);
}

void insert_char(EditorState &ed, int c) {
    ed.cache_lines[ed.cy].insert(ed.cx, 1, c);
    ed.cx++;
    ed.dirty = true;
}

void del_char(EditorState &ed) {
    if (ed.cx == 0 && ed.cy > 0) {
        ed.cx = ed.cache_lines[ed.cy-1].size();
        ed.cache_lines[ed.cy];
        ed.cache_lines.erase(ed.cache_lines.begin() + ed.cy);
        ed.cy--;
        ed.dirty = true;
    } else if (ed.cx > 0) {
        ed.cache_lines[ed.cy].erase(ed.cx-1, 1);
        ed.cx--;
        ed.dirty = true;
    }
}

void insert_newline(EditorState &ed) {
    ed.cache_lines.insert(ed.cache_lines.begin() + ed.cy + 1, ed.cache_lines[ed.cy].substr(ed.cx));
    ed.cache_lines[ed.cy] = ed.cache_lines[ed.cy].substr(0, ed.cx);
    ed.cy++;
    ed.cx = 0;
    ed.dirty = true;
}

string prompt(EditorState &ed, const string &msg, string def = "") {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    echo();
    curs_set(1);
    move(rows-2, 0);
    clrtoeol();
    mvprintw(rows-2, 0, "%s", msg.c_str());
    char buf[256] = {0};
    getnstr(buf, 255);
    noecho();
    curs_set(1);
    string s = buf;
    if (s.empty()) return def;
    return s;
}

void do_search(EditorState& ed, const string& word) {
    ed.search_word = word;
    ed.search_results.clear();
    ed.search_idx = 0;
    if (word.empty()) return;
    for (int i = 0; i < (int)ed.cache_lines.size(); ++i) {
        string& line = ed.cache_lines[i];
        size_t pos = 0;
        while ((pos = line.find(word, pos)) != string::npos) {
            ed.search_results.push_back({i, (int)pos});
            pos += word.length();
        }
    }
}

void goto_search(EditorState &ed, int rows) {
    ed.search_flash = true;
    if (ed.last_search_time && ed.search_idx < (int)ed.search_results.size()) {
        int sy = ed.search_results[ed.search_idx].first;
        int sx = ed.search_results[ed.search_idx].second;
        // 这里加调试输出
        set_status(ed, "跳转到: 行=" + to_string(sy) + " 列=" + to_string(sx));
        ed.cy = sy;
        ed.cx = sx;
        int screen_rows = rows - 3;
        if (ed.cy < ed.rowoff) ed.rowoff = ed.cy;
        if (ed.cy >= ed.rowoff + screen_rows) ed.rowoff = ed.cy - (screen_rows-1);
    }
}

void draw_help(int rows, int cols) {
    clear();
    int y = 1;
    mvprintw(y++, 2, "SEditor Help");
    y++;
    mvprintw(y++, 2, "^O Save    ^X Exit    ^C Cancel    ^F Find");
    mvprintw(y++, 2, "^G Help    Arrows Move    Mouse Wheel Scroll");
    mvprintw(y++, 2, "");
    mvprintw(y++, 2, "Find: Press ^ next, ^C to cancel");
    mvprintw(y++, 2, "Exit: If modified, ^X then Enter to save and exit, ^X to force exit, ^C to cancel");
    mvprintw(y++, 2, "");
    mvprintw(y++, 2, "Syntax highlighting: cpp/py/js/java/json");
    mvprintw(y++, 2, "");
    mvprintw(y++, 2, "Press any key to return to the editor...");
    refresh();
    getch();
}

void editor_loop(EditorState &ed) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    MEVENT event;
    while (1) {
        if (ed.loading) {
            clear();
            mvprintw(1, 2, "Loading, please wait...");
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (ed.search_flash && ((clock() - ed.last_search_time) > (CLOCKS_PER_SEC))) {
            ed.search_flash = false;
        }

        draw_rows(ed, rows, cols);
        draw_status(ed, rows, cols);
        draw_msg(ed, rows, cols);
        draw_shortcuts(rows, cols);
        move(ed.cy - ed.rowoff, ed.cx);
        refresh();

        int c = getch();
        if (c == KEY_MOUSE) {
            if (getmouse(&event) == OK) {
                if (event.bstate & BUTTON4_PRESSED) {
                    if (ed.cy > 0) ed.cy--;
                }
                if (event.bstate & BUTTON5_PRESSED) {
                    if (ed.cy < (int)ed.cache_lines.size() - 1) ed.cy++;
                }
                ed.cx = min(ed.cx, (int)ed.cache_lines[ed.cy].size());
                if (ed.cy < ed.rowoff) ed.rowoff = ed.cy;
                int screen_rows = rows-3;
                if (ed.cy >= ed.rowoff + screen_rows) ed.rowoff = ed.cy - (screen_rows-1);
            }
            continue;
        }
        else if (c == 7) { // ^G
            draw_help(rows, cols);
            continue;
        }
        else if (c == 6) { // ^F
    string prompt_word = ed.search_word.empty() ? "Find" : "Find(" + ed.search_word + ")";
    string word = prompt(ed, prompt_word + ":", ed.search_word);

    // 如果输入内容和当前search_word一样，也跳到下一个
    if ((word.empty() && !ed.search_word.empty()) || word == ed.search_word) {
        if (!ed.search_results.empty()) {
            ed.search_idx = (ed.search_idx + 1) % ed.search_results.size();
            goto_search(ed, rows);
        }
    } else if (!word.empty()) {
        do_search(ed, word);
        if (!ed.search_results.empty()) {
            ed.search_idx = 0;
            goto_search(ed, rows);
        } else {
            set_status(ed, "Not found");
        }
    }
    continue;
}
        else if (c == 24) { // ^X
    if (ed.dirty) {
        set_status(ed, "File modified. Save? (Enter=Yes, ^X=No, ^C=Cancel)");
        draw_status(ed, rows, cols);
        draw_msg(ed, rows, cols);
        int ch = getch();
        if (ch == '\n' || ch == '\r') { // Enter保存
            string fname = prompt(ed, "File Name", ed.filename);
            save_file(ed, fname);
            break;
        } else if (ch == 24) { // ^X强制退出
            break;
        } else if (ch == 3) { // ^C取消
            set_status(ed, "Cancel");
            continue;
        } else {
            set_status(ed, "Cancel");
            continue;
        }
    } else {
        break; // 没有修改直接退出
    }
}
        else if (c == 15) { // ^O
    if (!ed.filename.empty()) {
        save_file(ed, ed.filename);
    }
}
        else if (c == KEY_UP || c == KEY_DOWN || c == KEY_LEFT || c == KEY_RIGHT) {
            editor_move_cursor(ed, c, rows, cols);
        }
        else if (c == KEY_BACKSPACE || c == 127 || c == 8) {
            del_char(ed);
        }
        else if (c == '\n') {
            insert_newline(ed);
        }
        else if (c == 3) { // ^C
            set_status(ed, "Cancel");
        }
        else if (isprint(c)) {
            insert_char(ed, c);
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s filename\n", argv[0]);
        return 1;
    }
    EditorState ed;
    initscr();
    raw();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(1);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_BLUE, -1);   // 关键字
        init_pair(2, COLOR_GREEN, -1);  // 字符串
        init_pair(3, COLOR_CYAN, -1);   // 注释
        init_pair(4, COLOR_MAGENTA, -1);// 数字
        init_pair(5, COLOR_YELLOW, -1); // 搜索高亮
    }

    open_file(ed, argv[1]);
    editor_loop(ed);

    endwin();
    return 0;
}