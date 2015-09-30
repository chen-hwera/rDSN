# include <dsn/cpp/perf_test_helper.h>

# ifdef __TITLE__
# undef __TITLE__
# endif
# define __TITLE__ "perf.test.helper"

# define INVALID_DURATION_US 0xdeadbeefdeadbeefULL

namespace dsn {
    namespace service {
        
        perf_client_helper::perf_client_helper()
        {
            _case_count = 0;
            _live_rpc_count = 0;

            if (!read_config("task..default", _default_opts))
            {
                dassert(false, "read configuration failed for section [task..default]");
            }

            if (_default_opts.perf_test_concurrency.size() == 0)
            {
                const int ccs[] = { 1, 10, 100, 1000 };
                for (size_t i = 0; i < sizeof(ccs) / sizeof(int); i++)
                    _default_opts.perf_test_concurrency.push_back(ccs[i]);
            }

            if (_default_opts.perf_test_timeouts_ms.size() == 0)
            {
                const int timeouts_ms[] = { 10000 };
                for (size_t i = 0; i < sizeof(timeouts_ms) / sizeof(int); i++)
                    _default_opts.perf_test_timeouts_ms.push_back(timeouts_ms[i]);
            }

            if (_default_opts.perf_test_payload_bytes.size() == 0)
            {
                const int payload_bytes[] = { 1024,  64 * 1024, 512*1024, 1024*1024 };
                for (size_t i = 0; i < sizeof(payload_bytes) / sizeof(int); i++)
                    _default_opts.perf_test_payload_bytes.push_back(payload_bytes[i]);
            }
        }

        void perf_client_helper::load_suite_config(perf_test_suite& s)
        {
            perf_test_opts opt;
            if (!read_config(s.config_section, opt, &_default_opts))
            {
                dassert(false, "read configuration failed for section [%s]", s.config_section);
            }

            s.cases.clear();
            for (auto& bytes : opt.perf_test_payload_bytes)
            {
                int last_index = static_cast<int>(opt.perf_test_timeouts_ms.size()) - 1;
                for (int i = last_index; i >= 0; i--)
                {
                    for (auto& cc : opt.perf_test_concurrency)
                    {
                        perf_test_case c;
                        c.id = ++_case_count;
                        c.seconds = opt.perf_test_seconds;
                        c.payload_bytes = bytes;
                        c.timeout_ms = opt.perf_test_timeouts_ms[i];
                        c.concurrency = cc;
                        s.cases.push_back(c);
                    }
                }
            }
        }

        void perf_client_helper::start(const std::vector<perf_test_suite>& suits)
        {
            _suits = suits;
            _current_suit_index = -1;
            _current_case_index = 0xffffff;            

            start_next_case();
        }

        void* perf_client_helper::prepare_send_one()
        {
            uint64_t nts_ns = ::dsn_now_ns();
            ++_live_rpc_count;
            return (void*)(size_t)(nts_ns);
        }

        void perf_client_helper::end_send_one(void* context, error_code err)
        {
            uint64_t start_ts = (uint64_t)(context);
            uint64_t nts_ns = ::dsn_now_ns();
                        
            if (err != ERR_OK)
            {
                if (err == ERR_TIMEOUT)
                    _current_case->timeout_rounds++;
                else
                    _current_case->error_rounds++;
            }
            else
            {
                _current_case->succ_rounds++;

                auto d = nts_ns - start_ts;
                _current_case->succ_rounds_sum_ns += d;
                if (d < _current_case->min_latency_ns)
                    _current_case->min_latency_ns = d;
                if (d > _current_case->max_latency_ns)
                    _current_case->max_latency_ns = d;
            }

            // if completed
            if (_quiting_current_case)
            {
                if (--_live_rpc_count == 0)
                    finalize_case();
                return;
            }
            else if (nts_ns >= _case_end_ts_ns)
            {
                _quiting_current_case = true;
                if (--_live_rpc_count == 0)
                    finalize_case();
                return;
            }
            else
            {
                --_live_rpc_count;
            }
            
            // continue further waves
            if (_current_case->concurrency == 0)
            {
                // exponentially increase
                _suits[_current_suit_index].send_one(_current_case->payload_bytes);
                _suits[_current_suit_index].send_one(_current_case->payload_bytes);
            }
            else
            {
                // maintain fixed concurrent number
                while (!_quiting_current_case && _live_rpc_count <= _current_case->concurrency)
                {
                    _suits[_current_suit_index].send_one(_current_case->payload_bytes);
                }
            }
        }

        void perf_client_helper::finalize_case()
        {
            dassert(_live_rpc_count == 0, "all live requests must be completed");

            uint64_t nts = dsn_now_ns();
            auto& suit = _suits[_current_suit_index];
            auto& cs = suit.cases[_current_case_index];

            cs.succ_qps = (double)cs.succ_rounds / ((double)(nts - _case_start_ts_ns) / 1000.0 / 1000.0 / 1000.0);
            cs.succ_throughput_MB_s = (double)cs.succ_rounds * (double)cs.payload_bytes / 1024.0 / 1024.0 / ((double)(nts - _case_start_ts_ns) / 1000.0 / 1000.0 / 1000.0);
            cs.succ_latency_avg_ns = (double)cs.succ_rounds_sum_ns / (double)cs.succ_rounds;

            std::stringstream ss;
            ss << "TEST " << _name << "(" << cs.id << "/" << _case_count << ")::"
                << "  concurency: " << cs.concurrency
                << ", timeout(ms): " << cs.timeout_ms
                << ", payload(byte): " << cs.payload_bytes
                << ", tmo/err/suc(#): " << cs.timeout_rounds << "/" << cs.error_rounds << "/" << cs.succ_rounds
                << ", latency(ns): " << cs.succ_latency_avg_ns << "(avg), "
                << cs.min_latency_ns << "(min), "
                << cs.max_latency_ns << "(max)"
                << ", qps: " << cs.succ_qps << "#/s"
                << ", thp: " << cs.succ_throughput_MB_s << "MB/s"                
                ;

            dwarn(ss.str().c_str());

            start_next_case();
        }


        void perf_client_helper::start_next_case()
        {
            ++_current_case_index;

            // switch to next suit
            if (_current_suit_index == -1
                || _current_case_index >= (int)_suits[_current_suit_index].cases.size())
            {
                _current_suit_index++;
                _current_case_index = 0;

                if (_current_suit_index >= (int)_suits.size())
                {
                    std::stringstream ss;
                    ss << ">>>>>>>>>>>>>>>>>>>" << std::endl;

                    for (auto& s : _suits)
                    {
                        for (auto& cs : s.cases)
                        {
                            ss << "TEST " << _name << "(" << cs.id << "/" << _case_count << ")::"
                                << "  concurency: " << cs.concurrency
                                << ", timeout(ms): " << cs.timeout_ms
                                << ", payload(byte): " << cs.payload_bytes
                                << ", tmo/err/suc(#): " << cs.timeout_rounds << "/" << cs.error_rounds << "/" << cs.succ_rounds
                                << ", latency(ns): " << cs.succ_latency_avg_ns << "(avg), "
                                << cs.min_latency_ns << "(min), "
                                << cs.max_latency_ns << "(max)"
                                << ", qps: " << cs.succ_qps << "#/s"
                                << ", thp: " << cs.succ_throughput_MB_s << "MB/s"
                                << std::endl;
                                ;
                        }
                    }

                    dwarn(ss.str().c_str());
                    return;
                }
            }

            std::this_thread::sleep_for(std::chrono::seconds(2));

            // get next case
            auto& suit = _suits[_current_suit_index];
            auto& cs = suit.cases[_current_case_index];
            cs.timeout_rounds = 0;
            cs.error_rounds = 0;
            cs.max_latency_ns = 0;
            cs.min_latency_ns = UINT64_MAX;

            // setup for the case
            _current_case = &cs;
            _name = suit.name;
            _timeout_ms = cs.timeout_ms;
            _case_start_ts_ns = dsn_now_ns();
            _case_end_ts_ns = _case_start_ts_ns + (uint64_t)cs.seconds * 1000 * 1000 * 1000;
            _quiting_current_case = false;
            dassert(_live_rpc_count == 0, "all live requests must be completed");

            std::stringstream ss;
            ss << "TEST " << _name << "(" << cs.id << "/" << _case_count << ")::"
                << "  concurrency " << _current_case->concurrency
                << ", timeout(ms) " << _current_case->timeout_ms
                << ", payload(byte) " << _current_case->payload_bytes;
            dwarn(ss.str().c_str());

            // start
            suit.send_one(_current_case->payload_bytes);
        }
    }
}


