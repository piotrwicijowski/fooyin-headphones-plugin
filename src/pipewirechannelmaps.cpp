// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipewirechannelmaps.h"

#ifdef HEADPHONES_HAVE_PIPEWIRE
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QTimer>

#include <pipewire/pipewire.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#endif

namespace Fooyin::Headphones {

class PipewireChannelMaps::Impl
{
public:
#ifdef HEADPHONES_HAVE_PIPEWIRE
    struct Node
    {
        Impl* owner;
        pw_node* proxy {nullptr};
        spa_hook listener {};
        QByteArray name;
        QStringList positions;

        ~Node()
        {
            if(proxy) {
                spa_hook_remove(&listener);
                pw_proxy_destroy(reinterpret_cast<pw_proxy*>(proxy));
            }
        }
    };

    explicit Impl(PipewireChannelMaps& owner)
        : m_owner {owner}
    {
        static std::once_flag initialised;
        std::call_once(initialised, [] { pw_init(nullptr, nullptr); });
        m_loop = pw_main_loop_new(nullptr);
        if(!m_loop) {
            return;
        }
        m_context = pw_context_new(pw_main_loop_get_loop(m_loop), nullptr, 0);
        m_core    = m_context ? pw_context_connect(m_context, nullptr, 0) : nullptr;
        if(!m_core) {
            return; // No native server; never try another server or start one.
        }
        static const pw_core_events coreEvents = [] {
            pw_core_events events {};
            events.version = PW_VERSION_CORE_EVENTS;
            events.error   = [](void* data, uint32_t id, int, int, const char*) {
                if(id == PW_ID_CORE) {
                    auto& self = *static_cast<Impl*>(data);
                    self.m_nodes.clear();
                    self.changed();
                    if(self.m_notifier) {
                        self.m_notifier->setEnabled(false);
                    }
                }
            };
            return events;
        }();
        pw_core_add_listener(m_core, &m_coreListener, &coreEvents, this);
        m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
        static const pw_registry_events registryEvents {
            .version = PW_VERSION_REGISTRY_EVENTS,
            .global =
                [](void* data, uint32_t id, uint32_t, const char* type, uint32_t version, const spa_dict* props) {
                    auto& self             = *static_cast<Impl*>(data);
                    const auto* mediaClass = props ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : nullptr;
                    if(std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !mediaClass
                       || std::strcmp(mediaClass, "Audio/Sink") != 0) {
                        return;
                    }
                    auto node   = std::make_unique<Node>();
                    node->owner = &self;
                    node->proxy = static_cast<pw_node*>(
                        pw_registry_bind(self.m_registry, id, type, std::min(version, uint32_t {PW_VERSION_NODE}), 0));
                    if(!node->proxy) {
                        return;
                    }
                    static const pw_node_events nodeEvents {
                        .version = PW_VERSION_NODE_EVENTS,
                        .info =
                            [](void* data, const pw_node_info* info) {
                                auto& node = *static_cast<Node*>(data);
                                if(!(info->change_mask & PW_NODE_CHANGE_MASK_PROPS) || !info->props) {
                                    return;
                                }
                                const auto* name      = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
                                const auto* positions = spa_dict_lookup(info->props, "audio.position");
                                const auto nextName   = name ? QByteArray {name} : QByteArray {};
                                const auto nextPositions
                                    = QString::fromUtf8(positions ? positions : "")
                                          .split(QRegularExpression {QStringLiteral("[\\[\\],\\s]+")},
                                                 Qt::SkipEmptyParts);
                                if(node.name != nextName || node.positions != nextPositions) {
                                    node.name      = nextName;
                                    node.positions = nextPositions;
                                    node.owner->changed();
                                }
                            },
                        .param = nullptr,
                    };
                    pw_node_add_listener(node->proxy, &node->listener, &nodeEvents, node.get());
                    self.m_nodes.emplace(id, std::move(node));
                },
            .global_remove =
                [](void* data, uint32_t id) {
                    auto& self = *static_cast<Impl*>(data);
                    if(self.m_nodes.erase(id)) {
                        self.changed();
                    }
                },
        };
        pw_registry_add_listener(m_registry, &m_registryListener, &registryEvents, this);
        m_notifier
            = std::make_unique<QSocketNotifier>(pw_loop_get_fd(pw_main_loop_get_loop(m_loop)), QSocketNotifier::Read);
        QObject::connect(m_notifier.get(), &QSocketNotifier::activated, &m_owner, [this] {
            auto* loop = pw_main_loop_get_loop(m_loop);
            pw_loop_enter(loop);
            const int result = pw_loop_iterate(loop, 0); // Never wait on the GUI/audio pump.
            pw_loop_leave(loop);
            if(result < 0) {
                m_notifier->setEnabled(false);
                m_nodes.clear();
                changed();
            }
        });
    }

    ~Impl()
    {
        m_notifier.reset();
        m_nodes.clear();
        if(m_registry) {
            spa_hook_remove(&m_registryListener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_registry));
        }
        if(m_core) {
            spa_hook_remove(&m_coreListener);
            pw_core_disconnect(m_core);
        }
        if(m_context) {
            pw_context_destroy(m_context);
        }
        if(m_loop) {
            pw_main_loop_destroy(m_loop);
        }
    }

    std::optional<QStringList> positions(const QByteArray& id) const
    {
        std::optional<QStringList> result;
        for(const auto& [nodeId, node] : m_nodes) {
            if(node->name == id) {
                if(result) {
                    return {}; // Ambiguous node names are not an explicit device identity.
                }
                result = node->positions;
            }
        }
        return result;
    }

    bool isConnected() const
    {
        return m_core != nullptr;
    }

private:
    void changed()
    {
        if(!m_notificationPending) {
            m_notificationPending = true;
            QTimer::singleShot(0, &m_owner, [this] {
                m_notificationPending = false;
                Q_EMIT m_owner.mapsChanged();
            });
        }
    }

    PipewireChannelMaps& m_owner;
    pw_main_loop* m_loop {nullptr};
    pw_context* m_context {nullptr};
    pw_core* m_core {nullptr};
    pw_registry* m_registry {nullptr};
    spa_hook m_coreListener {}, m_registryListener {};
    std::unique_ptr<QSocketNotifier> m_notifier;
    std::map<uint32_t, std::unique_ptr<Node>> m_nodes;
    bool m_notificationPending {false};
#else
    explicit Impl(PipewireChannelMaps&) { }
    bool isConnected() const
    {
        return false;
    }
    std::optional<QStringList> positions(const QByteArray&) const
    {
        return {};
    }
#endif
};

PipewireChannelMaps::PipewireChannelMaps(QObject* parent)
    : QObject {parent}
    , m_impl {std::make_unique<Impl>(*this)}
{
}

PipewireChannelMaps::~PipewireChannelMaps() = default;

bool PipewireChannelMaps::isConnected() const
{
    return m_impl->isConnected();
}

std::optional<QStringList> PipewireChannelMaps::positions(const QByteArray& deviceId) const
{
    return m_impl->positions(deviceId);
}

} // namespace Fooyin::Headphones
