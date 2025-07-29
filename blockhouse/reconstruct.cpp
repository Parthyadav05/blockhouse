// reconstruct.cpp
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <optional>
#include <cstdint>
#include <unordered_map>
#include <map>
#include <vector>
#include <algorithm>
#include <limits>

static constexpr int64_t PRICE_UNDEF = std::numeric_limits<int64_t>::max();
static constexpr double PRICE_SCALE = 1e9;

enum class Action : char { A='A', M='M', C='C', R='R', T='T', F='F', N='N' };
enum class Side   : char { B='B', A='A', N='N' };

struct MboMessage {
    std::string ts_recv;
    std::string ts_event;
    uint8_t    rtype{};
    uint16_t   publisher_id{};
    uint32_t   instrument_id{};
    Action     action{};
    Side       side{};
    int        depth{};
    int64_t    price{};
    uint32_t   size{};
    uint8_t    flags{};
    int32_t    ts_in_delta{};
    uint32_t   sequence{};
    std::string symbol;
    uint64_t   order_id{};

    static std::optional<MboMessage> parse(const std::string& line) {
        std::stringstream ss(line);
        MboMessage m;
        std::string field;

        if (!std::getline(ss, m.ts_recv, ',')) {
            return std::nullopt;
        }
        if (!std::getline(ss, m.ts_event, ',')) {
            return std::nullopt;
        }
        if (!(ss >> m.rtype)) {
            return std::nullopt;
        }
        ss.ignore(1, ',');

        if (!(ss >> m.publisher_id)) {
            return std::nullopt;
        }
        ss.ignore(1, ',');

        if (!(ss >> m.instrument_id)) {
            return std::nullopt;
        }
        ss.ignore(1, ',');

        if (!std::getline(ss, field, ',') || field.size() != 1) {
            return std::nullopt;
        }
        m.action = static_cast<Action>(field[0]);

        if (!std::getline(ss, field, ',' ) || field.size() != 1) {
            return std::nullopt;
        }
        m.side = static_cast<Side>(field[0]);

        if (!(ss >> m.depth)) {
            return std::nullopt;
        }
        ss.ignore(1, ',');

        if (!std::getline(ss, field, ',')) {
            return std::nullopt;
        }
        if (field.empty()) {
            m.price = PRICE_UNDEF;
        } else {
            m.price = static_cast<int64_t>(std::stold(field) * PRICE_SCALE);
        }

        if (!(ss >> m.size)) {
            return std::nullopt;
        }
        ss.ignore(1, ',');

        if (!(ss >> m.order_id)) {
            return std::nullopt;
        }
        ss.ignore(1, ',');

        if (!(ss >> m.flags)) {
            return std::nullopt;
        }
        ss.ignore(1, ',');

        if (!(ss >> m.ts_in_delta)) {
            return std::nullopt;
        }
        ss.ignore(1, ',');

        if (!(ss >> m.sequence)) {
            return std::nullopt;
        }
        ss.ignore(1, ',');

        if (!std::getline(ss, m.symbol, ',')) {
            return std::nullopt;
        }

        if (!(ss >> m.order_id)) {
            return std::nullopt;
        }

        return m;
    }
};

struct PriceLevel {
    int64_t price;
    uint32_t size;
    uint32_t count;
};

class OrderBook {
    std::unordered_map<uint64_t, MboMessage> orders_;
    std::map<int64_t, std::vector<const MboMessage*>> bids_;
    std::map<int64_t, std::vector<const MboMessage*>> offers_;

    void clear() {
        orders_.clear();
        bids_.clear();
        offers_.clear();
    }

    void add(const MboMessage& m) {
        auto& lvl = (m.side == Side::B ? bids_ : offers_);
        if (m.flags & (1u << 6)) {
            lvl.clear();
            lvl[m.price] = { &m };
        } else {
            orders_.emplace(m.order_id, m);
            auto& msg = orders_.at(m.order_id);
            lvl[m.price].push_back(&msg);
        }
    }

    void cancel(const MboMessage& m) {
        auto it = orders_.find(m.order_id);
        if (it == orders_.end()) {
            return;
        }
        auto& msg = it->second;
        msg.size = msg.size > m.size ? msg.size - m.size : 0;
        auto& lvl = (m.side == Side::B ? bids_ : offers_);
        auto& vec = lvl[msg.price];
        vec.erase(std::remove(vec.begin(), vec.end(), &msg), vec.end());
        if (msg.size) {
            vec.push_back(&msg);
        } else {
            orders_.erase(it);
        }
        if (vec.empty()) {
            lvl.erase(msg.price);
        }
    }

    void modify(const MboMessage& m) {
        auto it = orders_.find(m.order_id);
        if (it == orders_.end()) {
            add(m);
            return;
        }
        auto& msg = it->second;
        auto& lvl = (msg.side == Side::B ? bids_ : offers_);
        if (msg.price != m.price) {
            auto& vec = lvl[msg.price];
            vec.erase(std::remove(vec.begin(), vec.end(), &msg), vec.end());
            if (vec.empty()) {
                lvl.erase(msg.price);
            }
            msg = m;
            lvl[m.price].push_back(&msg);
        } else if (msg.size < m.size) {
            auto& vec = lvl[msg.price];
            vec.erase(std::remove(vec.begin(), vec.end(), &msg), vec.end());
            msg = m;
            vec.push_back(&msg);
        } else {
            msg = m;
        }
    }

public:
    void apply(const MboMessage& m) {
        switch (m.action) {
            case Action::R: clear(); break;
            case Action::A: add(m);  break;
            case Action::C: cancel(m); break;
            case Action::M: modify(m); break;
            default: break;
        }
    }

    std::vector<PriceLevel> snapshot(size_t depth = 10) const {
        std::vector<PriceLevel> result;
        result.reserve(depth * 2);
        auto fill = [&](auto& lvl, bool rev) {
            size_t cnt = 0;
            if (rev) {
                for (auto it = lvl.rbegin(); it != lvl.rend() && cnt < depth; ++it, ++cnt) {
                    uint32_t sz = 0, ct = 0;
                    for (auto p : it->second) {
                        if (!(p->flags & (1u << 6))) {
                            sz += p->size;
                            ++ct;
                        }
                    }
                    result.push_back({ it->first, sz, ct });
                }
            } else {
                for (auto it = lvl.begin(); it != lvl.end() && cnt < depth; ++it, ++cnt) {
                    uint32_t sz = 0, ct = 0;
                    for (auto p : it->second) {
                        if (!(p->flags & (1u << 6))) {
                            sz += p->size;
                            ++ct;
                        }
                    }
                    result.push_back({ it->first, sz, ct });
                }
            }
            for (; cnt < depth; ++cnt) {
                result.push_back({ PRICE_UNDEF, 0, 0 });
            }
        };
        fill(bids_, true);
        fill(offers_, false);
        return result;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        return EXIT_FAILURE;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        return EXIT_FAILURE;
    }
    OrderBook book;
    std::string line;
    while (std::getline(in, line)) {
        auto opt = MboMessage::parse(line);
        if (!opt) {
            continue;
        }
        const auto& m = *opt;
        book.apply(m);
        auto snap = book.snapshot();
        std::cout << m.ts_recv << ',' << m.ts_event << ",10," << m.publisher_id << ','
                  << m.instrument_id << ',' << static_cast<char>(m.action) << ','
                  << static_cast<char>(m.side) << ',' << m.depth << ','
                  << (m.price == PRICE_UNDEF ? "" : std::to_string(m.price / PRICE_SCALE))
                  << ',' << m.size << ',' << static_cast<int>(m.flags) << ','
                  << m.ts_in_delta << ',' << m.sequence;
        for (auto& p : snap) {
            std::cout << ','
                      << (p.price == PRICE_UNDEF ? "" : std::to_string(p.price / PRICE_SCALE))
                      << ',' << p.size << ',' << p.count;
        }
        std::cout << ',' << m.symbol << ',' << m.order_id << '\n';
    }
    return EXIT_SUCCESS;
}
