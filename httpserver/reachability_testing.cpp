
#include "reachability_testing.h"
#include "loki_logger.h"

using std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace loki {

namespace detail {

reach_record_t::reach_record_t() {
    this->first_failure = steady_clock::now();
    this->last_failure = this->first_failure;
}

} // namespace detail

/// How long to wait until reporting unreachable nodes to Lokid
constexpr std::chrono::minutes UNREACH_GRACE_PERIOD = 120min;

bool reachability_records_t::should_report_as(const sn_pub_key_t& sn, ReportType type) {

    LOKI_LOG(trace, "should_report_as");

    using std::chrono::duration_cast;
    using std::chrono::minutes;

    const auto it = offline_nodes_.find(sn);

    if (it == offline_nodes_.end()) {
        // no record, we must have recordered this node as reachable already
        return false;
    }

    const auto& record = it->second;

    const bool reachable = record.http_ok && record.zmq_ok;

    if (type == ReportType::GOOD) {
        // Only report as reachable if both ports are reachable
        return reachable;
    } else {

        if (reachable) {
            // Not sure if this happens, but check just in case
            return false;
        }

        // Only report as unreachable if it has been unreachable for a long time

        const auto elapsed = record.last_failure - record.first_failure;
        const auto elapsed_min = duration_cast<minutes>(elapsed).count();
        LOKI_LOG(debug, "[reach] First time failed {} minutes ago", elapsed_min);

        if (it->second.reported) {
            LOKI_LOG(debug, "[reach]  Already reported node: {}", sn);
            // TODO: Might still want to report as unreachable since this status
            // gets reset to `true` on Lokid restart
            return false;
        } else if (elapsed > UNREACH_GRACE_PERIOD) {
            LOKI_LOG(debug, "[reach] Will REPORT {} to Lokid!", sn);
            return true;
        } else {
            // No need to report yet
            return false;
        }

    }

}

void reachability_records_t::check_incoming_tests(time_point_t reset_time) {

    constexpr auto MAX_TIME_WITHOUT_PING = PING_PEERS_INTERVAL * 18;

    const auto now = std::chrono::steady_clock::now();

    const auto last_http = std::max(reset_time, latest_incoming_http_);
    const auto http_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_http);

    LOKI_LOG(debug, "Last reset or pinged via http: {}s", http_elapsed.count());

    if (http_elapsed > MAX_TIME_WITHOUT_PING) {

        if (latest_incoming_http_.time_since_epoch() == 0s) {
            LOKI_LOG(warn, "Have NEVER received http pings!");
        } else {
            LOKI_LOG(
                warn,
                "Have not received http pings for a long time! Last time was: "
                "{} mins ago.",
                std::chrono::duration_cast<std::chrono::minutes>(http_elapsed).count());
        }

        this->http_ok = false;
        LOKI_LOG(warn, "Please check your http port. Not being reachable over http may result in a deregistration!");
    } else if (!this->http_ok) {
        this->http_ok = true;
        LOKI_LOG(info, "Http port is back to OK");
    }

    const auto last_lmq = std::max(reset_time, latest_incoming_lmq_);
    const auto lmq_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_lmq);

    LOKI_LOG(debug, "Last reset or pinged via lmq: {}s", lmq_elapsed.count());

    if (lmq_elapsed > MAX_TIME_WITHOUT_PING) {
        if (latest_incoming_lmq_.time_since_epoch() == 0s) {
            LOKI_LOG(warn, "Have NEVER received lmq pings!");
        } else {
            LOKI_LOG(warn,
                     "Have not received lmq pings for a long time! Last time "
                     "was: {} mins ago",
                     std::chrono::duration_cast<std::chrono::minutes>(lmq_elapsed).count());
        }

        this->lmq_ok = false;
        LOKI_LOG(warn, "Please check your lmq port. Not being reachable over lmq may result in a deregistration!");
    } else if (!this->lmq_ok) {
        this->lmq_ok = true;
        LOKI_LOG(info, "Lmq port is back to OK");
    }
}

void reachability_records_t::record_reachable(const sn_pub_key_t& sn, ReachType type, bool val) {

    LOKI_LOG(trace, "record_reachable");

    const auto it = offline_nodes_.find(sn);

    const bool no_record = it == offline_nodes_.end();

    if (no_record) {

        if (val) {
            // The node is good and there is no record, so do nothing
            LOKI_LOG(debug, "[reach] Node is reachable via {} (no record) {}",
                     type == ReachType::HTTP ? "HTTP" : "ZMQ", sn);
        } else {

            detail::reach_record_t record;

            if (type == ReachType::HTTP) {
                LOKI_LOG(debug, "[reach] Adding a new node to UNREACHABLE via HTTP: {}", sn);
                record.http_ok = false;
            } else if (type == ReachType::ZMQ) {
                LOKI_LOG(debug, "[reach] Adding a new node to UNREACHABLE via ZMQ: {}", sn);
                record.zmq_ok = false;
            }

            offline_nodes_.insert({sn, record});
        }
    } else {

        auto& record = it->second;

        const bool reachable_before = record.http_ok && record.zmq_ok;
        // Sometimes we might still have this entry even if the node has become reachable again

        if (type == ReachType::HTTP) {
            LOKI_LOG(debug, "[reach] node {} is {} via HTTP", sn, val ? "OK" : "UNREACHABLE");
            record.http_ok = val;
        } else if (type == ReachType::ZMQ) {
            LOKI_LOG(debug, "[reach] node {} is {} via ZMQ", sn, val ? "OK" : "UNREACHABLE");
            record.zmq_ok = val;
        }

        if (!val) {
            LOKI_LOG(debug,
                     "[reach] Node is ALREADY known to be UNREACHABLE: {}, "
                     "http_ok: {}, "
                     "zmq_ok: {}",
                     sn, record.http_ok, record.zmq_ok);

            const auto now = steady_clock::now();

            if (reachable_before) {
                record.first_failure = now;
            }

            record.last_failure = now;
        }
    }
}

bool reachability_records_t::expire(const sn_pub_key_t& sn) {

    bool erased = offline_nodes_.erase(sn);
    if (erased)
        LOKI_LOG(debug, "[reach] Removed entry for {}", sn);

    return erased;
}

void reachability_records_t::set_reported(const sn_pub_key_t& sn) {

    const auto it = offline_nodes_.find(sn);
    if (it != offline_nodes_.end()) {
        it->second.reported = true;
    }
}

boost::optional<sn_pub_key_t> reachability_records_t::next_to_test() {

    const auto it = std::min_element(
        offline_nodes_.begin(), offline_nodes_.end(),
        [&](const auto& lhs, const auto& rhs) {
            return lhs.second.last_failure < rhs.second.last_failure;
        });

    if (it == offline_nodes_.end()) {
        return boost::none;
    } else {

        LOKI_LOG(debug, "Selecting to be re-tested: {}", it->first);

        return it->first;
    }
}

} // namespace loki
