#include <eosio/randpa_plugin/randpa_plugin.hpp>

#include <eosio/chain/plugin_interface.hpp>
#include <eosio/http_client_plugin/http_client_plugin.hpp>
#include <eosio/randpa_plugin/network_messages.hpp>
#include <eosio/randpa_plugin/prefix_chain_tree.hpp>
#include <eosio/randpa_plugin/randpa.hpp>
#include <eosio/telemetry_plugin/telemetry_plugin.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/smart_ref_impl.hpp>

#include <boost/algorithm/string.hpp>

#include <atomic>
#include <chrono>
#include <queue>

namespace eosio {

using namespace std::chrono_literals;
using namespace eosio::chain;
using namespace eosio::chain::plugin_interface;
using namespace randpa_finality;

static appbase::abstract_plugin& _randpa_plugin = app().register_plugin<randpa_plugin>();

static constexpr uint32_t net_message_types_base = 100;

class randpa_plugin_impl {
public:
    randpa _randpa;

    channels::irreversible_block::channel_type::handle _on_irb_handle;
    channels::accepted_block::channel_type::handle     _on_accepted_block_handle;
    net_plugin::new_peer::channel_type::handle         _on_new_peer_handle;

    //

    randpa_plugin_impl() {}

    template <typename T>
    static constexpr uint32_t get_net_msg_type(const T& msg = {}) {
        return net_message_types_base + randpa_net_msg_data::tag<T>::value;
    }

    static std::set<public_key_type> get_bp_keys(block_state_ptr s) {
        std::set<public_key_type> producer_keys;
        for (const auto& elem : s->active_schedule.producers) {
            producer_keys.insert(elem.block_signing_key);
        }
        return producer_keys;
    }

    void start() {
        auto in_net_ch = std::make_shared<net_channel>();
        auto out_net_ch = std::make_shared<net_channel>();
        auto ev_ch = std::make_shared<event_channel>();
        auto finality_ch = std::make_shared<finality_channel>();

        _randpa
            .set_in_net_channel(in_net_ch)
            .set_out_net_channel(out_net_ch)
            .set_event_channel(ev_ch)
            .set_finality_channel(finality_ch);

        subscribe<handshake_msg>(in_net_ch);
        subscribe<handshake_ans_msg>(in_net_ch);
        subscribe<prevote_msg>(in_net_ch);
        subscribe<precommit_msg>(in_net_ch);
        subscribe<proof_msg>(in_net_ch);
        subscribe<finality_notice_msg>(in_net_ch);
        subscribe<finality_req_proof_msg>(in_net_ch);

        _on_accepted_block_handle = app().get_channel<channels::accepted_block>()
            .subscribe( [ev_ch, this]( block_state_ptr s ) {
                app().get_plugin<telemetry_plugin>().update_gauge("randpa_queue_size", _randpa.get_message_queue().size());
                app().get_plugin<telemetry_plugin>().update_gauge("head_block_num", app().get_plugin<chain_plugin>().chain().head_block_num());
                ev_ch->send(randpa_event { on_accepted_block_event {
                    s->id,
                    s->header.previous,
                    s->block_signing_key,
                    get_bp_keys(s),
                    is_sync(s)
                }});
            });

        _on_irb_handle = app().get_channel<channels::irreversible_block>()
            .subscribe( [ev_ch]( block_state_ptr s ) {
                app().get_plugin<telemetry_plugin>().update_gauge("lib_block_num", s->block_num);
                ev_ch->send(randpa_event { on_irreversible_event { s->id } });
            });

        _on_new_peer_handle = app().get_channel<net_plugin::new_peer>()
            .subscribe( [ev_ch]( uint32_t ses_id ) {
                ev_ch->send(randpa_event { on_new_peer_event { ses_id } });
            });

        out_net_ch->subscribe([this](const randpa_net_msg& msg) {
            const auto data = msg.data;
            switch (data.which()) {
            case randpa_net_msg_data::tag<prevote_msg>::value:
                send(msg.ses_id, data.get<prevote_msg>());
                app().get_plugin<telemetry_plugin>().update_counter("randpa_net_out_prevote_cnt");
                break;
            case randpa_net_msg_data::tag<precommit_msg>::value:
                send(msg.ses_id, data.get<precommit_msg>());
                app().get_plugin<telemetry_plugin>().update_counter("randpa_net_out_precommit_cnt");
                break;
            case randpa_net_msg_data::tag<proof_msg>::value:
                send(msg.ses_id, data.get<proof_msg>());
                app().get_plugin<telemetry_plugin>().update_counter("randpa_net_out_proof_cnt");
                break;
            case randpa_net_msg_data::tag<handshake_msg>::value:
                send(msg.ses_id, data.get<handshake_msg>());
                app().get_plugin<telemetry_plugin>().update_counter("randpa_net_out_handshake_cnt");
                break;
            case randpa_net_msg_data::tag<handshake_ans_msg>::value:
                send(msg.ses_id, data.get<handshake_ans_msg>());
                app().get_plugin<telemetry_plugin>().update_counter("randpa_net_out_handshake_ans_cnt");
                break;
            case randpa_net_msg_data::tag<finality_notice_msg>::value:
                send(msg.ses_id, data.get<finality_notice_msg>());
                app().get_plugin<telemetry_plugin>().update_counter("randpa_net_out_finality_notice_cnt");
                break;
            case randpa_net_msg_data::tag<finality_req_proof_msg>::value:
                send(msg.ses_id, data.get<finality_req_proof_msg>());
                app().get_plugin<telemetry_plugin>().update_counter("randpa_net_out_finality_req_proof_cnt");
                break;
            default:
                randpa_wlog("randpa message sent, but handler not found, type: ${type}", ("type", data.which()));
                break;
            }
            app().get_plugin<telemetry_plugin>().update_counter("randpa_net_out_total_cnt");
        });

        finality_ch->subscribe([](const block_id_type& block_id) {
            app().get_io_service().post([block_id = block_id]() {
                app().get_plugin<chain_plugin>()
                    .chain()
                    .bft_finalize(block_id);
            });
        });

        app().get_plugin<telemetry_plugin>().add_gauge("randpa_queue_size");
        app().get_plugin<telemetry_plugin>().add_gauge("head_block_num");
        app().get_plugin<telemetry_plugin>().add_gauge("lib_block_num");

        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_in_total_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_in_prevote_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_in_precommit_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_in_proof_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_in_handshake_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_in_handshake_ans_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_in_finality_notice_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_in_finality_req_proof_cnt");

        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_out_total_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_out_prevote_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_out_precommit_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_out_proof_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_out_handshake_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_out_handshake_ans_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_out_finality_notice_cnt");
        app().get_plugin<telemetry_plugin>().add_counter("randpa_net_out_finality_req_proof_cnt");

        _randpa.start(copy_fork_db());
    }

    static bool is_sync(const block_state_ptr& block) {
        return fc::time_point::now() - block->header.timestamp > fc::seconds(2);
    }

    static prefix_tree_ptr copy_fork_db() {
        const auto& ctrl = app().get_plugin<chain_plugin>().chain();
        const auto lib_id = ctrl.last_irreversible_block_id();

        randpa_dlog("Initializing prefix_chain_tree with ${lib_id}", ("lib_id", lib_id));
        auto root = std::make_unique<tree_node>(tree_node{lib_id});
        prefix_tree_ptr tree(new prefix_tree(std::move(root)));

        randpa_dlog("Copying master chain from fork_db");
        auto current_block = ctrl.head_block_state();

        vector<block_state_ptr> blocks;
        while (current_block && current_block->id != lib_id) {
            blocks.push_back(current_block);
            current_block = ctrl.fetch_block_state_by_id(current_block->prev());
        }
        std::reverse(blocks.begin(), blocks.end());

        auto base_block = lib_id;
        for (const auto& block_ptr : blocks) {
            const auto block_id = block_ptr->id;
            tree->insert(chain_type{base_block, {block_ptr->id}},
                         block_ptr->block_signing_key,
                         get_bp_keys(block_ptr));
            base_block = block_id;
        }
        randpa_dlog("Successfully copied ${amount} blocks", ("amount", blocks.size()));
        return tree;
    }

    void stop() {
        _randpa.stop();
    }

    template <typename T>
    static void send(uint32_t ses_id, const T& msg) {
        app().post(priority::high, [ses_id, msg]() {
            app().get_plugin<net_plugin>().send(ses_id, get_net_msg_type(msg), msg);
        });
    }

    template <typename T>
    static void subscribe(const net_channel_ptr& ch) {
        app().get_plugin<net_plugin>().subscribe<T>(
            get_net_msg_type<T>(),
            [ch](uint32_t ses_id, const T& msg) {
                ch->send(randpa_net_msg { ses_id, msg, fc::time_point::now() });
                switch (randpa_net_msg_data::tag<T>::value) {
                case randpa_net_msg_data::tag<prevote_msg>::value:
                    app().get_plugin<telemetry_plugin>().update_counter("randpa_net_in_prevote_cnt");
                    break;
                case randpa_net_msg_data::tag<precommit_msg>::value:
                    app().get_plugin<telemetry_plugin>().update_counter("randpa_net_in_precommit_cnt");
                    break;
                case randpa_net_msg_data::tag<proof_msg>::value:
                    app().get_plugin<telemetry_plugin>().update_counter("randpa_net_in_proof_cnt");
                    break;
                case randpa_net_msg_data::tag<handshake_msg>::value:
                    app().get_plugin<telemetry_plugin>().update_counter("randpa_net_in_handshake_cnt");
                    break;
                case randpa_net_msg_data::tag<handshake_ans_msg>::value:
                    app().get_plugin<telemetry_plugin>().update_counter("randpa_net_in_handshake_ans_cnt");
                    break;
                case randpa_net_msg_data::tag<finality_notice_msg>::value:
                    app().get_plugin<telemetry_plugin>().update_counter("randpa_net_in_finality_notice_cnt");
                    break;
                case randpa_net_msg_data::tag<finality_req_proof_msg>::value:
                    app().get_plugin<telemetry_plugin>().update_counter("randpa_net_in_finality_req_proof_cnt");
                    break;
                }
                app().get_plugin<telemetry_plugin>().update_counter("randpa_net_in_total_cnt");
            }
        );
    }
};


randpa_plugin::randpa_plugin() : my(new randpa_plugin_impl()) {}
randpa_plugin::~randpa_plugin() {}

static signature_provider_type make_key_signature_provider(const private_key_type& key) {
    return [key](const chain::digest_type& digest) {
        return key.sign(digest);
    };
}

static signature_provider_type make_keosd_signature_provider(const string& url_str, const public_key_type& pubkey) {
    const fc::url keosd_url =
        boost::algorithm::starts_with(url_str, "unix://")
        ?
            //send the entire string after unix:// to http_plugin. It'll auto-detect which part
            // is the unix socket path, and which part is the url to hit on the server
            fc::url("unix", url_str.substr(7), ostring(), ostring(), ostring(), ostring(), ovariant_object(), fc::optional<uint16_t>())
        :
            fc::url(url_str)
        ;

    return [keosd_url, pubkey](const chain::digest_type& digest) {
        fc::variant params;
        fc::to_variant(std::make_pair(digest, pubkey), params);
        auto deadline = fc::time_point::maximum();
        return app().get_plugin<http_client_plugin>().get_client().post_sync(keosd_url, params, deadline).as<chain::signature_type>();
    };
}

void randpa_plugin::plugin_initialize(const variables_map& options) {
    if (options.count("producer-name") > 0) {
        my->_randpa.set_type_block_producer();
    } else {
        // this is a full node; don't parse --signature-provider options unless producer name is passed
        // @see plugins/producer_plugin/producer_plugin.cpp
        return;
    }
    FC_ASSERT(options.count("signature-provider") > 0, "no 'signature-provider' options for a block producer node");

    // parse --signature-provider options
    const auto key_spec_pair_vector = options["signature-provider"].as<vector<std::string>>();
    std::vector<signature_provider_type> sig_provs;
    std::vector<public_key_type> pub_keys;

    for (const std::string& key_spec_pair : key_spec_pair_vector) {
        try {
            auto delim = key_spec_pair.find("=");
            EOS_ASSERT(delim != std::string::npos, plugin_config_exception, "Missing \"=\" in the key spec pair");
            auto pub_key_str = key_spec_pair.substr(0, delim);
            auto spec_str = key_spec_pair.substr(delim + 1);

            auto spec_delim = spec_str.find(":");
            EOS_ASSERT(spec_delim != std::string::npos, plugin_config_exception,
                       "Missing \":\" in the key spec pair");
            auto spec_type_str = spec_str.substr(0, spec_delim);
            auto spec_data = spec_str.substr(spec_delim + 1);

            auto pubkey = public_key_type(pub_key_str);

            pub_keys.push_back(pubkey);
            if (spec_type_str == "KEY") {
                sig_provs.push_back(make_key_signature_provider(private_key_type(spec_data)));
            } else if (spec_type_str == "KEOSD") {
                sig_provs.push_back(make_keosd_signature_provider(spec_data, pubkey));
            }
        } catch (const fc::exception &) {
            randpa_elog("Malformed signature provider ${val}", ("val", key_spec_pair));
            return;
        }
    }
    my->_randpa.set_signature_providers(sig_provs, pub_keys);
}

void randpa_plugin::plugin_startup() {
    handle_sighup();
    my->start();
}

void randpa_plugin::plugin_shutdown() {
    my->stop();
}

void randpa_plugin::handle_sighup() {
    fc::logger::update(randpa_logger_name, randpa_logger);
}

} // namespace eosio
