// Postflop heads-up NLHE GTO solver (Discounted CFR, vectorized range-vs-range)
// build: see build.sh (macOS/Linux) or build.bat (Windows)
// run:   ./solver config.txt
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
using namespace std;

// ---------------------------------------------------------------- cards
static const char* RANKS = "23456789TJQKA";
static const char* SUITS = "cdhs";
static int rankOf(char c) { const char* p = strchr(RANKS, toupper(c)); return p && c ? int(p - RANKS) : -1; }
static int suitOf(char c) { const char* p = strchr(SUITS, tolower(c)); return p && c ? int(p - SUITS) : -1; }
static string cardStr(int c) { string s; s += RANKS[c >> 2]; s += SUITS[c & 3]; return s; }
[[noreturn]] static void die(const string& m) { fprintf(stderr, "ERROR: %s\n", m.c_str()); exit(1); }

static int straightHi(int m) {
    for (int hi = 12; hi >= 4; hi--) if (((m >> (hi - 4)) & 31) == 31) return hi;
    if ((m & 0x100F) == 0x100F) return 3;
    return -1;
}
static int popc(int m) { int c = 0; while (m) { m &= m - 1; c++; } return c; }
static int top5(int m) { while (popc(m) > 5) m &= m - 1; return m; }
static int eval7(const int* cs, int n) {
    int rc[13] = {0}, sm[4] = {0}, sc[4] = {0}, rm = 0;
    for (int i = 0; i < n; i++) { int r = cs[i] >> 2, s = cs[i] & 3; rc[r]++; sm[s] |= 1 << r; sc[s]++; rm |= 1 << r; }
    int fm = 0;
    for (int s = 0; s < 4; s++) if (sc[s] >= 5) fm = sm[s];
    if (fm) { int sf = straightHi(fm); if (sf >= 0) return (8 << 20) | sf; }
    int quad = -1, trips = -1, p1 = -1, p2 = -1;
    for (int r = 12; r >= 0; r--) {
        if (rc[r] == 4) { if (quad < 0) quad = r; }
        else if (rc[r] == 3) { if (trips < 0) trips = r; else if (p1 < 0) p1 = r; else if (p2 < 0) p2 = r; }
        else if (rc[r] == 2) { if (p1 < 0) p1 = r; else if (p2 < 0) p2 = r; }
    }
    auto kick = [&](int ex1, int ex2, int k) { int v = 0; for (int r = 12; r >= 0 && k; r--) if (rc[r] && r != ex1 && r != ex2) { v = (v << 4) | r; k--; } return v; };
    if (quad >= 0) return (7 << 20) | (quad << 4) | kick(quad, -1, 1);
    if (trips >= 0 && p1 >= 0) return (6 << 20) | (trips << 4) | p1;
    if (fm) return (5 << 20) | top5(fm);
    int st = straightHi(rm);
    if (st >= 0) return (4 << 20) | st;
    if (trips >= 0) return (3 << 20) | (trips << 8) | kick(trips, -1, 2);
    if (p2 >= 0) return (2 << 20) | (p1 << 8) | (p2 << 4) | kick(p1, p2, 1);
    if (p1 >= 0) return (1 << 20) | (p1 << 12) | kick(p1, -1, 3);
    return top5(rm);
}

// ---------------------------------------------------------------- config
struct Config {
    vector<int> board;
    double pot = 10, stack = 100;
    string range[2];
    vector<double> bet[2][3], raise_[2][3];
    bool addAllin = true;
    double allinThreshold = 0.67;
    int raiseCap = 3;
    int iterations = 300, checkEvery = 10, threads = 0, exportStreets = 1;
    double target = 0.5; // % of pot
    double rake = 0, rakeCap = 0;
    string output = "result.js";
    bool iso = true, compress = true;
    // 6-max matchup preset
    string matchup, potType = "srp", rangesFile = "ranges_6max.txt", names[2] = {"OOP", "IP"};
    double stackBB = 100, openSize = 2.5, sbOpenSize = 3, tbIP = 3, tbOOP = 4, fbMult = 2.2;
    bool potSet = false, stackSet = false;
} cfg;

static string trim(const string& s) { size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n"); return a == string::npos ? "" : s.substr(a, b - a + 1); }
static vector<double> parseList(const string& v) {
    vector<double> r; string t = v; for (char& c : t) if (c == ',' || c == '%') c = ' ';
    stringstream ss(t); double x; while (ss >> x) r.push_back(x); return r;
}
// Fill ranges / pot / stack from a 6-max preflop scenario (values given explicitly in the config win).
static void resolveMatchup(const char* cfgPath) {
    static const char* PRE[6] = {"utg", "hj", "co", "btn", "sb", "bb"};
    auto low = [](string x) { for (char& c : x) c = (char)tolower(c); return x; };
    auto up = [](string x) { for (char& c : x) c = (char)toupper(c); return x; };
    string m = low(cfg.matchup); size_t v = m.find("vs");
    if (v == string::npos) die("matchup must look like: BTN vs BB");
    auto pre = [&](const string& x) -> int {
        if (x == "lj" || x == "ep") return 0; if (x == "mp") return 1;
        for (int i = 0; i < 6; i++) if (x == PRE[i]) return i;
        die("unknown position '" + x + "' (use UTG, HJ, CO, BTN, SB, BB)");
    };
    int ia = pre(trim(m.substr(0, v))), ib = pre(trim(m.substr(v + 2)));
    if (ib <= ia) die("matchup: first position is the open-raiser and must act before the second (order: UTG, HJ, CO, BTN, SB, BB)");
    string a = PRE[ia], b = PRE[ib];
    auto post = [](int i) { return i >= 4 ? i - 4 : i + 2; };
    bool aOOP = post(ia) < post(ib);
    ifstream f(cfg.rangesFile);
    if (!f) { string d = cfgPath; size_t sl = d.find_last_of("/\\"); f.open((sl == string::npos ? string() : d.substr(0, sl + 1)) + cfg.rangesFile); }
    if (!f) die("cannot open ranges file " + cfg.rangesFile);
    map<string, string> R; string line;
    while (getline(f, line)) {
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line = line.substr(3);
        size_t h = line.find('#'); if (h != string::npos) line = line.substr(0, h);
        size_t e = line.find('='); if (e == string::npos) continue;
        R[low(trim(line.substr(0, e)))] = trim(line.substr(e + 1));
    }
    auto rel = [](bool oop) { return string(oop ? "oop" : "ip"); };
    auto look = [&](const string& kind, const string& x, const string& y, bool xOOP) -> string {
        vector<string> c = {x + "_vs_" + y, x + "_vs_" + rel(!xOOP), rel(xOOP) + "_vs_" + y, x, rel(xOOP)};
        for (auto& k : c) { auto it = R.find(kind + "_" + k); if (it != R.end()) return it->second; }
        die("ranges file has no entry for " + kind + "_" + c[0]);
    };
    double open = ia == 4 ? cfg.sbOpenSize : cfg.openSize, tb = open * (aOOP ? cfg.tbIP : cfg.tbOOP), fb = tb * cfg.fbMult, x;
    string pt = low(cfg.potType), ra, rb, desc;
    if (pt == "srp") { ra = look("rfi", a, b, aOOP); rb = look("call", b, a, !aOOP); x = open; desc = "single-raised pot"; }
    else if (pt == "3bp") { ra = look("callvs3bet", a, b, aOOP); rb = look("3bet", b, a, !aOOP); x = tb; desc = "3-bet pot"; }
    else if (pt == "4bp") { ra = look("4bet", a, b, aOOP); rb = look("callvs4bet", b, a, !aOOP); x = fb; desc = "4-bet pot"; }
    else die("pot_type must be srp, 3bp or 4bp");
    double dead = (ia != 4 && ib != 4 ? 0.5 : 0) + (ib != 5 ? 1.0 : 0);
    if (cfg.stackBB - x <= 0) die("stack_bb too small for this pot type");
    if (!cfg.potSet) cfg.pot = 2 * x + dead;
    if (!cfg.stackSet) cfg.stack = cfg.stackBB - x;
    int pa = aOOP ? 0 : 1;
    if (cfg.range[pa].empty()) cfg.range[pa] = ra;
    if (cfg.range[1 - pa].empty()) cfg.range[1 - pa] = rb;
    cfg.names[pa] = up(a); cfg.names[1 - pa] = up(b);
    printf("6-max matchup: %s vs %s, %s (each player put in %.4g bb, dead money %.4g bb)\n", up(a).c_str(), up(b).c_str(), desc.c_str(), x, dead);
    printf("  OOP = %s, IP = %s\n", cfg.names[0].c_str(), cfg.names[1].c_str());
}
static bool truthy(const string& v) { return v == "true" || v == "1" || v == "yes" || v == "on"; }
static void loadConfig(const char* path) {
    ifstream f(path); if (!f) die(string("cannot open ") + path);
    string line;
    const char* pn[2] = {"oop", "ip"}; const char* sn[3] = {"flop", "turn", "river"};
    while (getline(f, line)) {
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line = line.substr(3);
        size_t h = line.find('#'); if (h != string::npos) line = line.substr(0, h);
        size_t e = line.find('='); if (e == string::npos) continue;
        string k = trim(line.substr(0, e)), v = trim(line.substr(e + 1));
        for (char& c : k) c = tolower(c);
        bool ok = true;
        if (k == "board") {
            string t; for (char c : v) if (!isspace(c) && c != ',') t += c;
            if (t.size() % 2) die("bad board");
            for (size_t i = 0; i < t.size(); i += 2) { int r = rankOf(t[i]), s = suitOf(t[i + 1]); if (r < 0 || s < 0) die("bad board card"); cfg.board.push_back(r * 4 + s); }
        } else if (k == "pot") { if (!v.empty()) { cfg.pot = atof(v.c_str()); cfg.potSet = true; } }
        else if (k == "stack") { if (!v.empty()) { cfg.stack = atof(v.c_str()); cfg.stackSet = true; } }
        else if (k == "range_oop") cfg.range[0] = v;
        else if (k == "range_ip") cfg.range[1] = v;
        else if (k == "add_allin") cfg.addAllin = truthy(v);
        else if (k == "allin_threshold") cfg.allinThreshold = atof(v.c_str());
        else if (k == "raise_cap") cfg.raiseCap = atoi(v.c_str());
        else if (k == "iterations") cfg.iterations = atoi(v.c_str());
        else if (k == "check_every") cfg.checkEvery = max(1, atoi(v.c_str()));
        else if (k == "threads") cfg.threads = atoi(v.c_str());
        else if (k == "export_streets") cfg.exportStreets = atoi(v.c_str());
        else if (k == "target_exploitability") cfg.target = atof(v.c_str());
        else if (k == "rake") cfg.rake = atof(v.c_str());
        else if (k == "rake_cap") cfg.rakeCap = atof(v.c_str());
        else if (k == "output") cfg.output = v;
        else if (k == "isomorphism") cfg.iso = truthy(v);
        else if (k == "matchup") cfg.matchup = v;
        else if (k == "pot_type") cfg.potType = v;
        else if (k == "ranges_file") cfg.rangesFile = v;
        else if (k == "stack_bb") cfg.stackBB = atof(v.c_str());
        else if (k == "open_size") cfg.openSize = atof(v.c_str());
        else if (k == "sb_open_size") cfg.sbOpenSize = atof(v.c_str());
        else if (k == "threebet_ip_mult") cfg.tbIP = atof(v.c_str());
        else if (k == "threebet_oop_mult") cfg.tbOOP = atof(v.c_str());
        else if (k == "fourbet_mult") cfg.fbMult = atof(v.c_str());
        else if (k == "compress") cfg.compress = truthy(v);
        else {
            ok = false;
            for (int p = 0; p < 2; p++) for (int s = 0; s < 3; s++) {
                if (k == string(pn[p]) + "_" + sn[s] + "_bet") { cfg.bet[p][s] = parseList(v); ok = true; }
                if (k == string(pn[p]) + "_" + sn[s] + "_raise") { cfg.raise_[p][s] = parseList(v); ok = true; }
            }
        }
        if (!ok) fprintf(stderr, "warning: unknown key '%s'\n", k.c_str());
    }
    if (!cfg.matchup.empty()) resolveMatchup(path);
    if (cfg.board.size() < 3 || cfg.board.size() > 5) die("board must have 3-5 cards");
    set<int> u(cfg.board.begin(), cfg.board.end()); if (u.size() != cfg.board.size()) die("duplicate board card");
    if (cfg.threads <= 0) cfg.threads = max(1u, thread::hardware_concurrency());
}

// ---------------------------------------------------------------- ranges
struct Hand { uint8_t a, b; float w; };
static vector<Hand> H[2];
static vector<int> same_[2];
static map<pair<int, int>, float> rawRange[2];
static bool swapOK[4][4];
static vector<int> isoPerm[16][2]; // [s1*4+s2][player][hand] -> hand index after swapping suits s1,s2
static int swapSuit(int c, int s1, int s2) { int s = c & 3; return s == s1 ? (c & ~3) | s2 : s == s2 ? (c & ~3) | s1 : c; }

static void addCombo(map<pair<int, int>, float>& m, int c1, int c2, float w) { if (c1 == c2) return; if (c1 > c2) swap(c1, c2); m[{c1, c2}] = w; }
static void addClass(map<pair<int, int>, float>& m, int r1, int r2, char t, float w) {
    for (int s1 = 0; s1 < 4; s1++) for (int s2 = 0; s2 < 4; s2++) {
        if (r1 == r2) { if (s1 < s2) addCombo(m, r1 * 4 + s1, r2 * 4 + s2, w); continue; }
        if (t == 's' && s1 != s2) continue;
        if (t == 'o' && s1 == s2) continue;
        addCombo(m, r1 * 4 + s1, r2 * 4 + s2, w);
    }
}
struct Base { int r1, r2; char t; };
static Base parseBase(const string& s) {
    if (s.size() < 2) die("bad range token: " + s);
    Base b{rankOf(s[0]), rankOf(s[1]), 'b'};
    if (b.r1 < 0 || b.r2 < 0) die("bad range token: " + s);
    if (s.size() >= 3) { char c = tolower(s[2]); if (c != 's' && c != 'o') die("bad range token: " + s); b.t = c; }
    if (b.r1 < b.r2) swap(b.r1, b.r2);
    return b;
}
static void parseRange(const string& str, int p) {
    map<pair<int, int>, float> m;
    string t; for (char c : str) if (!isspace(c)) t += c;
    stringstream ss(t); string tok;
    while (getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        float w = 1; size_t col = tok.find(':');
        if (col != string::npos) { w = atof(tok.substr(col + 1).c_str()); tok = tok.substr(0, col); }
        if (tok.size() == 4 && suitOf(tok[1]) >= 0 && suitOf(tok[3]) >= 0 && rankOf(tok[0]) >= 0 && rankOf(tok[2]) >= 0) {
            addCombo(m, rankOf(tok[0]) * 4 + suitOf(tok[1]), rankOf(tok[2]) * 4 + suitOf(tok[3]), w); continue;
        }
        size_t dash = tok.find('-');
        if (dash != string::npos) {
            Base x = parseBase(tok.substr(0, dash)), y = parseBase(tok.substr(dash + 1));
            if (x.r1 == x.r2) { for (int r = min(x.r1, y.r1); r <= max(x.r1, y.r1); r++) addClass(m, r, r, 'b', w); }
            else { for (int r = min(x.r2, y.r2); r <= max(x.r2, y.r2); r++) addClass(m, x.r1, r, x.t, w); }
        } else if (tok.back() == '+') {
            Base x = parseBase(tok.substr(0, tok.size() - 1));
            if (x.r1 == x.r2) { for (int r = x.r1; r <= 12; r++) addClass(m, r, r, 'b', w); }
            else { for (int r = x.r2; r < x.r1; r++) addClass(m, x.r1, r, x.t, w); }
        } else { Base x = parseBase(tok); addClass(m, x.r1, x.r2, x.t, w); }
    }
    rawRange[p] = m;
    uint64_t bm = 0; for (int c : cfg.board) bm |= 1ull << c;
    for (auto& kv : m) {
        if (kv.second <= 0) continue;
        if ((bm >> kv.first.first & 1) || (bm >> kv.first.second & 1)) continue;
        H[p].push_back({(uint8_t)kv.first.first, (uint8_t)kv.first.second, kv.second});
    }
    if (H[p].empty()) die(string("empty range for ") + (p ? "IP" : "OOP"));
}

// ---------------------------------------------------------------- tree
enum { ACTION, CHANCE, FOLD, SHOWDOWN };
enum { K_FOLD, K_CHECK, K_CALL, K_BET, K_RAISE, K_ALLIN, K_CARD };
struct Node {
    uint8_t type, player, nA, exportable;
    float c[2];
    int aux;      // showdown table / chance denominator
    int first, nChild;
    void* reg = nullptr; void* ss = nullptr; // float[] or (compressed) int16[] / uint16[] with per-node scale
    float rsc = 0, ssc = 0;
};
struct Edge { int node; uint8_t kind; uint8_t card; uint8_t iso; float amt; }; // iso: 0 or suit-swap id+1 (shares previous representative's subtree)
static vector<Node> N;
static vector<Edge> E;
struct SD { vector<uint16_t> ord[2]; vector<int> str[2]; vector<int> board; };
static vector<SD> sds;
static map<vector<int>, int> sdIndex;

struct State { vector<int> board; double c[2]; double streetStart; int toAct; int nRaises; double lastInc; int depth; };
static int buildAction(State s);
static int newNode(int type, const State& s) {
    Node n{}; n.type = type; n.c[0] = s.c[0]; n.c[1] = s.c[1]; n.exportable = s.depth < cfg.exportStreets; n.first = 0; n.nChild = 0; n.aux = 0;
    N.push_back(n); return (int)N.size() - 1;
}
static int endStreet(State s) {
    if (s.board.size() == 5) {
        int id = newNode(SHOWDOWN, s);
        vector<int> b = s.board; sort(b.begin(), b.end());
        auto it = sdIndex.find(b);
        if (it == sdIndex.end()) { sds.push_back(SD()); sds.back().board = b; it = sdIndex.emplace(b, (int)sds.size() - 1).first; }
        N[id].aux = it->second; return id;
    }
    int id = newNode(CHANCE, s);
    N[id].aux = 52 - (int)s.board.size() - 4;
    vector<Edge> es;
    int mask[4] = {0}, rp[4];
    for (int c : s.board) mask[c & 3] |= 1 << (c >> 2);
    for (int a = 0; a < 4; a++) { rp[a] = a; if (cfg.iso) for (int b = 0; b < a; b++) if (mask[b] == mask[a] && swapOK[b][a]) { rp[a] = b; break; } }
    for (int c = 0; c < 52; c++) {
        if (find(s.board.begin(), s.board.end(), c) != s.board.end()) continue;
        if (rp[c & 3] != (c & 3)) continue; // isomorphic to a lower suit: no subtree of its own
        State t = s; t.board.push_back(c); t.toAct = 0; t.nRaises = 0; t.lastInc = 0; t.streetStart = s.c[0]; t.depth++;
        int ch = (s.c[0] >= cfg.stack - 1e-9) ? endStreet(t) : buildAction(t);
        es.push_back({ch, K_CARD, (uint8_t)c, 0, 0});
        for (int b = (c & 3) + 1; b < 4; b++) if (rp[b] == (c & 3)) es.push_back({ch, K_CARD, (uint8_t)((c & ~3) | b), (uint8_t)((c & 3) * 4 + b + 1), 0});
    }
    N[id].first = (int)E.size(); N[id].nChild = (int)es.size();
    E.insert(E.end(), es.begin(), es.end());
    return id;
}
static int buildAction(State s) {
    int id = newNode(ACTION, s);
    int p = s.toAct, o = 1 - p, st = (int)s.board.size() - 3;
    N[id].player = p;
    double toCall = s.c[o] - s.c[p], pot = cfg.pot + s.c[0] + s.c[1], left = cfg.stack - s.c[p];
    vector<Edge> es;
    auto addAgg = [&](const vector<double>& sizes, bool isRaise) {
        set<long long> seen;
        vector<pair<double, bool>> adds; // (chips added, isAllin)
        double base = isRaise ? toCall : 0, potRef = pot + base;
        for (double pct : sizes) {
            double by = pct / 100.0 * potRef;
            if (isRaise) by = max(by, s.lastInc);
            double add = base + by;
            if (add <= base + 1e-9) continue;
            bool ai = add >= left * cfg.allinThreshold - 1e-9;
            if (ai) add = left;
            adds.push_back({add, ai});
        }
        if (cfg.addAllin) adds.push_back({left, true});
        sort(adds.begin(), adds.end());
        for (auto& ad : adds) {
            long long key = llround(ad.first * 1000);
            if (!seen.insert(key).second) continue;
            State t = s; t.c[p] += ad.first; t.lastInc = t.c[p] - s.c[o]; t.nRaises++; t.toAct = o;
            int ch = buildAction(t);
            es.push_back({ch, (uint8_t)(ad.second ? K_ALLIN : (isRaise ? K_RAISE : K_BET)), 0, 0, (float)(isRaise || ad.second ? t.c[p] - s.streetStart : ad.first)});
        }
    };
    if (toCall > 1e-9) {
        { State t = s; int ch = newNode(FOLD, t); N[ch].player = p; es.push_back({ch, K_FOLD, 0, 0}); }
        { State t = s; t.c[p] = s.c[o]; int ch = endStreet(t); es.push_back({ch, K_CALL, 0, 0, (float)toCall}); }
        if (s.nRaises < cfg.raiseCap && left > toCall + 1e-9) addAgg(cfg.raise_[p][st], true);
    } else {
        { State t = s; int ch; if (p == 0) { t.toAct = 1; ch = buildAction(t); } else ch = endStreet(t); es.push_back({ch, K_CHECK, 0, 0}); }
        if (left > 1e-9 && cfg.raiseCap > 0) addAgg(cfg.bet[p][st], false);
    }
    N[id].first = (int)E.size(); N[id].nChild = N[id].nA = (int)es.size();
    E.insert(E.end(), es.begin(), es.end());
    return id;
}

// ---------------------------------------------------------------- solver
enum { M_CFR, M_BR, M_EV };
static float posD = 0, negD = 0.5f, ssD = 0;
static thread_local bool inPar = false;
struct Extra { vector<float> ev, reach; };
static unordered_map<int, Extra> extra;
static mutex extraMu;

static void validSum(int p, const float* ro, float* out) {
    int q = 1 - p; double tot = 0, cs[52] = {0};
    for (size_t i = 0; i < H[q].size(); i++) { double r = ro[i]; if (r == 0) continue; tot += r; cs[H[q][i].a] += r; cs[H[q][i].b] += r; }
    for (size_t h = 0; h < H[p].size(); h++) {
        int sm = same_[p][h];
        out[h] = (float)(tot - cs[H[p][h].a] - cs[H[p][h].b] + (sm >= 0 ? ro[sm] : 0));
    }
}
static float* fetchReg(const Node& nd, size_t cnt, vector<float>& buf) {
    if (!cfg.compress) return (float*)nd.reg;
    buf.resize(cnt); const int16_t* s = (const int16_t*)nd.reg; float sc = nd.rsc;
    for (size_t i = 0; i < cnt; i++) buf[i] = s[i] * sc;
    return buf.data();
}
static float* fetchSS(const Node& nd, size_t cnt, vector<float>& buf) {
    if (!cfg.compress) return (float*)nd.ss;
    buf.resize(cnt); const uint16_t* s = (const uint16_t*)nd.ss; float sc = nd.ssc;
    for (size_t i = 0; i < cnt; i++) buf[i] = s[i] * sc;
    return buf.data();
}
static void commitReg(Node& nd, const float* v, size_t cnt) {
    if (!cfg.compress) return;
    float mx = 0; for (size_t i = 0; i < cnt; i++) mx = max(mx, fabsf(v[i]));
    int16_t* d = (int16_t*)nd.reg; nd.rsc = mx / 32767.0f;
    if (mx <= 0) { memset(d, 0, cnt * 2); return; }
    float inv = 32767.0f / mx; for (size_t i = 0; i < cnt; i++) d[i] = (int16_t)lrintf(v[i] * inv);
}
static void commitSS(Node& nd, const float* v, size_t cnt) {
    if (!cfg.compress) return;
    float mx = 0; for (size_t i = 0; i < cnt; i++) mx = max(mx, v[i]);
    uint16_t* d = (uint16_t*)nd.ss; nd.ssc = mx / 65535.0f;
    if (mx <= 0) { memset(d, 0, cnt * 2); return; }
    float inv = 65535.0f / mx; for (size_t i = 0; i < cnt; i++) d[i] = (uint16_t)lrintf(v[i] * inv);
}
static void getStrategy(const float* src, int A, int n, float* sig) {
    for (int h = 0; h < n; h++) {
        float s = 0;
        for (int a = 0; a < A; a++) { float x = src[a * n + h]; if (x > 0) s += x; }
        if (s > 0) for (int a = 0; a < A; a++) { float x = src[a * n + h]; sig[a * n + h] = x > 0 ? x / s : 0; }
        else for (int a = 0; a < A; a++) sig[a * n + h] = 1.0f / A;
    }
}
static double rakeOf(double pot) { if (cfg.rake <= 0) return 0; double r = pot * cfg.rake; return cfg.rakeCap > 0 ? min(r, cfg.rakeCap) : r; }

static void walk(int ni, int p, const float* ro, float* out, int mode) {
    Node& nd = N[ni];
    int q = 1 - p, n = (int)H[p].size(), m = (int)H[q].size();
    switch (nd.type) {
    case FOLD: {
        int folder = nd.player;
        double pay = folder == p ? -nd.c[p] : cfg.pot + nd.c[folder] - rakeOf(cfg.pot + 2 * nd.c[folder]);
        validSum(p, ro, out);
        for (int h = 0; h < n; h++) out[h] *= (float)pay;
        return;
    }
    case SHOWDOWN: {
        const SD& t = sds[nd.aux];
        const auto& T = t.ord[p]; const auto& O = t.ord[q]; const auto& sT = t.str[p]; const auto& sO = t.str[q];
        double c = nd.c[0], rk = rakeOf(cfg.pot + 2 * c);
        double W = cfg.pot + c - rk, Ti = (cfg.pot - rk) / 2, L = -c;
        vector<float> valid(n); validSum(p, ro, valid.data());
        memset(out, 0, sizeof(float) * n);
        int nt = (int)T.size(), no = (int)O.size();
        vector<double> win(nt);
        { double cs[52] = {0}, sum = 0; int j = 0;
          for (int i = 0; i < nt; i++) {
              while (j < no && sO[j] < sT[i]) { int h = O[j]; double r = ro[h]; sum += r; cs[H[q][h].a] += r; cs[H[q][h].b] += r; j++; }
              int h = T[i]; win[i] = sum - cs[H[p][h].a] - cs[H[p][h].b];
          } }
        { double cs[52] = {0}, sum = 0; int j = no - 1;
          for (int i = nt - 1; i >= 0; i--) {
              while (j >= 0 && sO[j] > sT[i]) { int h = O[j]; double r = ro[h]; sum += r; cs[H[q][h].a] += r; cs[H[q][h].b] += r; j--; }
              int h = T[i]; double lose = sum - cs[H[p][h].a] - cs[H[p][h].b];
              double tie = valid[h] - win[i] - lose;
              out[h] = (float)(win[i] * W + lose * L + tie * Ti);
          } }
        return;
    }
    case CHANCE: {
        int nc = nd.nChild; const Edge* es = &E[nd.first];
        auto doCard = [&](int k, float* acc, vector<float>& r2, vector<float>& tmp) {
            int card = es[k].card;
            bool any = false;
            for (int h = 0; h < m; h++) { r2[h] = (H[q][h].a == card || H[q][h].b == card) ? 0 : ro[h]; any |= r2[h] != 0; }
            if (!any) return;
            walk(es[k].node, p, r2.data(), tmp.data(), mode);
            for (int h = 0; h < n; h++) if (H[p][h].a != card && H[p][h].b != card) acc[h] += tmp[h];
            for (int j = k + 1; j < nc && es[j].iso; j++) { // isomorphic cards: same values with suits swapped
                const vector<int>& pm = isoPerm[es[j].iso - 1][p];
                for (int h = 0; h < n; h++) if (H[p][h].a != card && H[p][h].b != card) { int t = pm[h]; if (t >= 0) acc[t] += tmp[h]; }
            }
        };
        vector<int> reps; for (int k = 0; k < nc; k++) if (!es[k].iso) reps.push_back(k);
        int nr = (int)reps.size();
        memset(out, 0, sizeof(float) * n);
        if (!inPar && cfg.threads > 1) {
            atomic<int> next(0); mutex mu; vector<thread> th;
            int nt = min(cfg.threads, nr);
            for (int t = 0; t < nt; t++) th.emplace_back([&] {
                inPar = true;
                vector<float> acc(n, 0), r2(m), tmp(n);
                int k; while ((k = next++) < nr) doCard(reps[k], acc.data(), r2, tmp);
                lock_guard<mutex> g(mu); for (int h = 0; h < n; h++) out[h] += acc[h];
            });
            for (auto& t : th) t.join();
        } else {
            vector<float> r2(m), tmp(n);
            for (int k = 0; k < nr; k++) doCard(reps[k], out, r2, tmp);
        }
        float inv = 1.0f / nd.aux;
        for (int h = 0; h < n; h++) out[h] *= inv;
        return;
    }
    default: break;
    }
    // ACTION
    int A = nd.nA; const Edge* es = &E[nd.first];
    if (nd.player == p) {
        vector<float> ua((size_t)A * n);
        for (int a = 0; a < A; a++) walk(es[a].node, p, ro, &ua[(size_t)a * n], mode);
        if (mode == M_BR) {
            for (int h = 0; h < n; h++) { float b = ua[h]; for (int a = 1; a < A; a++) b = max(b, ua[(size_t)a * n + h]); out[h] = b; }
            return;
        }
        vector<float> sig((size_t)A * n), buf;
        float* rg = mode == M_CFR ? fetchReg(nd, (size_t)A * n, buf) : fetchSS(nd, (size_t)A * n, buf);
        getStrategy(rg, A, n, sig.data());
        for (int h = 0; h < n; h++) { float v = 0; for (int a = 0; a < A; a++) v += sig[(size_t)a * n + h] * ua[(size_t)a * n + h]; out[h] = v; }
        if (mode == M_CFR) {
            for (int a = 0; a < A; a++) { float* r = rg + (size_t)a * n; const float* u = &ua[(size_t)a * n];
                for (int h = 0; h < n; h++) { float x = r[h]; r[h] = x * (x > 0 ? posD : negD) + (u[h] - out[h]); } }
            commitReg(nd, rg, (size_t)A * n);
        } else if (nd.exportable) {
            Extra ex; ex.ev.resize(n); vector<float> v(n); validSum(p, ro, v.data());
            for (int h = 0; h < n; h++) ex.ev[h] = v[h] > 1e-9f ? out[h] / v[h] : 0;
            lock_guard<mutex> g(extraMu); extra[ni].ev = std::move(ex.ev);
        }
    } else {
        vector<float> sig((size_t)A * m), r2(m), tmp(n), buf;
        getStrategy(mode == M_CFR ? fetchReg(nd, (size_t)A * m, buf) : fetchSS(nd, (size_t)A * m, buf), A, m, sig.data());
        memset(out, 0, sizeof(float) * n);
        for (int a = 0; a < A; a++) {
            bool any = false; const float* s = &sig[(size_t)a * m];
            for (int h = 0; h < m; h++) { r2[h] = ro[h] * s[h]; any |= r2[h] != 0; }
            if (!any) continue;
            walk(es[a].node, p, r2.data(), tmp.data(), mode);
            for (int h = 0; h < n; h++) out[h] += tmp[h];
        }
        if (mode == M_CFR) {
            float* sb = fetchSS(nd, (size_t)A * m, buf);
            for (int a = 0; a < A; a++) { float* s = sb + (size_t)a * m; const float* g = &sig[(size_t)a * m];
                for (int h = 0; h < m; h++) s[h] = s[h] * ssD + ro[h] * g[h]; }
            commitSS(nd, sb, (size_t)A * m);
        } else if (mode == M_EV && nd.exportable) {
            lock_guard<mutex> g(extraMu); extra[ni].reach.assign(ro, ro + m);
        }
    }
}

static double Z = 0;
static double rootValue(int p, int mode) {
    int q = 1 - p; vector<float> ro(H[q].size()), out(H[p].size());
    for (size_t i = 0; i < H[q].size(); i++) ro[i] = H[q][i].w;
    walk(0, p, ro.data(), out.data(), mode);
    double v = 0; for (size_t h = 0; h < H[p].size(); h++) v += (double)H[p][h].w * out[h];
    return v / Z;
}

// ---------------------------------------------------------------- export
static string handStr(const Hand& h) { return cardStr(h.b) + cardStr(h.a); }
static void writeArr(FILE* f, const float* v, int n, const char* fmt = "%.3g") {
    fputc('[', f); for (int i = 0; i < n; i++) { if (i) fputc(',', f); float x = v[i]; if (fabsf(x) < 5e-4f) fputc('0', f); else fprintf(f, fmt, x); } fputc(']', f);
}
static void exportNode(FILE* f, int ni) {
    const Node& nd = N[ni];
    if (nd.type == FOLD) { fputs("{\"t\":\"f\"}", f); return; }
    if (nd.type == SHOWDOWN) { fputs("{\"t\":\"s\"}", f); return; }
    if (nd.type == CHANCE) {
        if (!(N[E[nd.first].node].exportable) || N[E[nd.first].node].type != ACTION) { fputs("{\"t\":\"c\",\"x\":1}", f); return; }
        fputs("{\"t\":\"c\",\"cards\":[", f);
        bool fst = true; string rc;
        for (int k = 0; k < nd.nChild; k++) if (!E[nd.first + k].iso) { fprintf(f, "%s\"%s\"", fst ? "" : ",", cardStr(E[nd.first + k].card).c_str()); fst = false; }
        fputs("],\"iso\":[", f); fst = true;
        for (int k = 0; k < nd.nChild; k++) { const Edge& e = E[nd.first + k]; if (!e.iso) { rc = cardStr(e.card); continue; }
            fprintf(f, "%s[\"%s\",\"%s\"]", fst ? "" : ",", cardStr(e.card).c_str(), rc.c_str()); fst = false; }
        fputs("],\"ch\":[", f); fst = true;
        for (int k = 0; k < nd.nChild; k++) if (!E[nd.first + k].iso) { if (!fst) fputc(',', f); fst = false; exportNode(f, E[nd.first + k].node); }
        fputs("]}", f); return;
    }
    int p = nd.player, n = (int)H[p].size(), A = nd.nA;
    fprintf(f, "{\"t\":\"a\",\"p\":%d,\"pot\":%.4g,\"A\":[", p, cfg.pot + nd.c[0] + nd.c[1]);
    for (int a = 0; a < A; a++) {
        const Edge& e = E[nd.first + a]; char buf[64];
        switch (e.kind) {
        case K_FOLD: snprintf(buf, 64, "Fold"); break; case K_CHECK: snprintf(buf, 64, "Check"); break;
        case K_CALL: snprintf(buf, 64, "Call %.4g", e.amt); break; case K_BET: snprintf(buf, 64, "Bet %.4g", e.amt); break;
        case K_RAISE: snprintf(buf, 64, "Raise %.4g", e.amt); break; default: snprintf(buf, 64, "All-in %.4g", e.amt);
        }
        fprintf(f, "%s\"%s\"", a ? "," : "", buf);
    }
    fputs("],\"S\":[", f);
    vector<float> sig((size_t)A * n), sbuf; getStrategy(fetchSS(nd, (size_t)A * n, sbuf), A, n, sig.data());
    for (int a = 0; a < A; a++) { if (a) fputc(',', f); writeArr(f, &sig[(size_t)a * n], n); }
    fputc(']', f);
    auto it = extra.find(ni);
    if (it != extra.end()) {
        if ((int)it->second.ev.size() == n) { fputs(",\"ev\":", f); writeArr(f, it->second.ev.data(), n, "%.4g"); }
        if ((int)it->second.reach.size() == n) { fputs(",\"r\":", f); writeArr(f, it->second.reach.data(), n); }
    }
    fputs(",\"ch\":[", f);
    for (int a = 0; a < A; a++) { if (a) fputc(',', f); exportNode(f, E[nd.first + a].node); }
    fputs("]}", f);
}

int main(int argc, char** argv) {
    loadConfig(argc > 1 ? argv[1] : "config.txt");
    parseRange(cfg.range[0], 0); parseRange(cfg.range[1], 1);
    for (int p = 0; p < 2; p++) {
        map<pair<int, int>, int> idx; for (size_t i = 0; i < H[1 - p].size(); i++) idx[{H[1 - p][i].a, H[1 - p][i].b}] = (int)i;
        for (auto& h : H[p]) { auto it = idx.find({h.a, h.b}); same_[p].push_back(it == idx.end() ? -1 : it->second); }
    }
    { vector<float> w(H[1].size()), v(H[0].size()); for (size_t i = 0; i < H[1].size(); i++) w[i] = H[1][i].w;
      validSum(0, w.data(), v.data()); for (size_t h = 0; h < H[0].size(); h++) Z += (double)H[0][h].w * v[h]; }
    if (Z <= 0) die("ranges have no compatible combos");

    string isoDesc;
    for (int s1 = 0; s1 < 4; s1++) for (int s2 = s1 + 1; s2 < 4; s2++) {
        bool ok = true;
        for (int p = 0; p < 2 && ok; p++) for (auto& kv : rawRange[p]) {
            if (kv.second <= 0) continue;
            int a = swapSuit(kv.first.first, s1, s2), b = swapSuit(kv.first.second, s1, s2); if (a > b) swap(a, b);
            auto it = rawRange[p].find({a, b});
            if (it == rawRange[p].end() || fabs(it->second - kv.second) > 1e-6) { ok = false; break; }
        }
        swapOK[s1][s2] = ok;
        if (!ok) continue;
        for (int p = 0; p < 2; p++) {
            map<pair<int, int>, int> idx; for (size_t i = 0; i < H[p].size(); i++) idx[{H[p][i].a, H[p][i].b}] = (int)i;
            for (auto& h : H[p]) { int a = swapSuit(h.a, s1, s2), b = swapSuit(h.b, s1, s2); if (a > b) swap(a, b);
                auto it = idx.find({a, b}); isoPerm[s1 * 4 + s2][p].push_back(it == idx.end() ? -1 : it->second); }
        }
    }
    auto t0 = chrono::steady_clock::now();
    auto secs = [&] { return chrono::duration<double>(chrono::steady_clock::now() - t0).count(); };
    State s; s.board = cfg.board; s.c[0] = s.c[1] = 0; s.streetStart = 0; s.toAct = 0; s.nRaises = 0; s.lastInc = 0; s.depth = 0;
    buildAction(s);
    size_t total = 0, nAct = 0;
    for (auto& nd : N) if (nd.type == ACTION) { total += (size_t)nd.nA * H[nd.player].size(); nAct++; }
    printf("Board: "); for (int c : cfg.board) printf("%s ", cardStr(c).c_str());
    printf("| pot %.4g, stack %.4g\n%s combos: %zu, %s combos: %zu\n", cfg.pot, cfg.stack, cfg.names[0].c_str(), H[0].size(), cfg.names[1].c_str(), H[1].size());
    size_t esz = cfg.compress ? 2 : 4;
    printf("Tree: %zu nodes (%zu decision nodes), %zu showdown boards, threads %d\n", N.size(), nAct, sds.size(), cfg.threads);
    printf("Isomorphism: %s, compression: %s, memory %.2f GB (+%.2f GB tree)\n", cfg.iso ? "on" : "off", cfg.compress ? "16-bit" : "off",
           total * 2.0 * esz / 1e9, (N.size() * sizeof(Node) + E.size() * sizeof(Edge)) / 1e9);
    fflush(stdout);
    char* regA = (char*)calloc(total, esz); char* ssA = (char*)calloc(total, esz);
    if (!regA || !ssA) die("out of memory - reduce bet sizes / ranges");
    { size_t off = 0; for (auto& nd : N) if (nd.type == ACTION) { nd.reg = regA + off * esz; nd.ss = ssA + off * esz; off += (size_t)nd.nA * H[nd.player].size(); } }
    { // showdown tables
        atomic<int> next(0); vector<thread> th;
        for (int t = 0; t < cfg.threads; t++) th.emplace_back([&] {
            int k; while ((k = next++) < (int)sds.size()) {
                SD& d = sds[k]; uint64_t bm = 0; for (int c : d.board) bm |= 1ull << c;
                for (int p = 0; p < 2; p++) {
                    vector<pair<int, int>> v;
                    for (size_t h = 0; h < H[p].size(); h++) {
                        if ((bm >> H[p][h].a & 1) || (bm >> H[p][h].b & 1)) continue;
                        int cs[7]; for (int i = 0; i < 5; i++) cs[i] = d.board[i]; cs[5] = H[p][h].a; cs[6] = H[p][h].b;
                        v.push_back({eval7(cs, 7), (int)h});
                    }
                    sort(v.begin(), v.end());
                    for (auto& x : v) { d.str[p].push_back(x.first); d.ord[p].push_back((uint16_t)x.second); }
                }
            }
        });
        for (auto& t : th) t.join();
    }
    printf("Setup done in %.1fs. Solving...\n", secs()); fflush(stdout);

    double expl = 1e9; int it;
    for (it = 1; it <= cfg.iterations; it++) {
        double tp = it - 1, a = pow(tp, 1.5);
        posD = (float)(a / (a + 1)); negD = 0.5f; ssD = (float)pow(tp / (tp + 1), 2.0);
        for (int p = 0; p < 2; p++) {
            int q = 1 - p; vector<float> ro(H[q].size()), out(H[p].size());
            for (size_t i = 0; i < H[q].size(); i++) ro[i] = H[q][i].w;
            walk(0, p, ro.data(), out.data(), M_CFR);
        }
        if (it % cfg.checkEvery == 0 || it == cfg.iterations) {
            double b0 = rootValue(0, M_BR), b1 = rootValue(1, M_BR);
            expl = (b0 + b1 - cfg.pot) / 2;
            printf("iter %4d  exploitability %.4f chips (%.3f%% pot)  %.1fs\n", it, expl, expl / cfg.pot * 100, secs()); fflush(stdout);
            if (expl / cfg.pot * 100 <= cfg.target) break;
        }
    }
    double e0 = rootValue(0, M_EV), e1 = rootValue(1, M_EV);
    printf("\nEV %s: %.4f   EV %s: %.4f   (sum %.4f, pot %.4g)\n", cfg.names[0].c_str(), e0, cfg.names[1].c_str(), e1, e0 + e1, cfg.pot);
    { // root summary
        const Node& r = N[0]; int n = (int)H[0].size(); vector<float> sig((size_t)r.nA * n), sbuf; getStrategy(fetchSS(r, (size_t)r.nA * n, sbuf), r.nA, n, sig.data());
        vector<float> w(H[1].size()), v(n); for (size_t i = 0; i < H[1].size(); i++) w[i] = H[1][i].w; validSum(0, w.data(), v.data());
        printf("%s root strategy:\n", cfg.names[0].c_str());
        for (int a = 0; a < r.nA; a++) {
            double f = 0; for (int h = 0; h < n; h++) f += (double)H[0][h].w * v[h] * sig[(size_t)a * n + h];
            const Edge& e = E[r.first + a];
            printf("  %-6s %-8.4g %6.2f%%\n", e.kind == K_CHECK ? "Check" : e.kind == K_BET ? "Bet" : "All-in", e.amt, f / Z * 100);
        }
    }
    FILE* f = fopen(cfg.output.c_str(), "w"); if (!f) die("cannot write output");
    fprintf(f, "window.RESULT={\"board\":[");
    for (size_t i = 0; i < cfg.board.size(); i++) fprintf(f, "%s\"%s\"", i ? "," : "", cardStr(cfg.board[i]).c_str());
    fprintf(f, "],\"names\":[\"%s\",\"%s\"]", cfg.names[0].c_str(), cfg.names[1].c_str());
    fprintf(f, ",\"pot\":%.6g,\"stack\":%.6g,\"iterations\":%d,\"exploitability\":%.6g,\"ev\":[%.6g,%.6g],\"hands\":[", cfg.pot, cfg.stack, min(it, cfg.iterations), expl, e0, e1);
    for (int p = 0; p < 2; p++) { fprintf(f, "%s[", p ? "," : ""); for (size_t h = 0; h < H[p].size(); h++) fprintf(f, "%s\"%s\"", h ? "," : "", handStr(H[p][h]).c_str()); fputc(']', f); }
    fputs("],\"weights\":[", f);
    for (int p = 0; p < 2; p++) { if (p) fputc(',', f); vector<float> w; for (auto& h : H[p]) w.push_back(h.w); writeArr(f, w.data(), (int)w.size()); }
    fputs("],\"root\":", f); exportNode(f, 0); fputs("};\n", f); fclose(f);
    printf("Saved %s (open viewer.html to browse). Total %.1fs\n", cfg.output.c_str(), secs());
    return 0;
}
