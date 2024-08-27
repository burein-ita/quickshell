#include "node.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/node.h>
#include <qcontainerfwd.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qtmetamacros.h>
#include <qtypes.h>
#include <spa/node/keys.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/pod/pod.h>
#include <spa/pod/vararg.h>
#include <spa/utils/dict.h>
#include <spa/utils/keys.h>
#include <spa/utils/type.h>

#include "device.hpp"

namespace qs::service::pipewire {

Q_LOGGING_CATEGORY(logNode, "quickshell.service.pipewire.node", QtWarningMsg);

QString PwAudioChannel::toString(Enum value) {
	switch (value) {
	case Unknown: return "Unknown";
	case NA: return "N/A";
	case Mono: return "Mono";
	case FrontCenter: return "Front Center";
	case FrontLeft: return "Front Left";
	case FrontRight: return "Front Right";
	case FrontLeftCenter: return "Front Left Center";
	case FrontRightCenter: return "Front Right Center";
	case FrontLeftWide: return "Front Left Wide";
	case FrontRightWide: return "Front Right Wide";
	case FrontCenterHigh: return "Front Center High";
	case FrontLeftHigh: return "Front Left High";
	case FrontRightHigh: return "Front Right High";
	case LowFrequencyEffects: return "Low Frequency Effects";
	case LowFrequencyEffects2: return "Low Frequency Effects 2";
	case LowFrequencyEffectsLeft: return "Low Frequency Effects Left";
	case LowFrequencyEffectsRight: return "Low Frequency Effects Right";
	case SideLeft: return "Side Left";
	case SideRight: return "Side Right";
	case RearCenter: return "Rear Center";
	case RearLeft: return "Rear Left";
	case RearRight: return "Rear Right";
	case RearLeftCenter: return "Rear Left Center";
	case RearRightCenter: return "Rear Right Center";
	case TopCenter: return "Top Center";
	case TopFrontCenter: return "Top Front Center";
	case TopFrontLeft: return "Top Front Left";
	case TopFrontRight: return "Top Front Right";
	case TopFrontLeftCenter: return "Top Front Left Center";
	case TopFrontRightCenter: return "Top Front Right Center";
	case TopSideLeft: return "Top Side Left";
	case TopSideRight: return "Top Side Right";
	case TopRearCenter: return "Top Rear Center";
	case TopRearLeft: return "Top Rear Left";
	case TopRearRight: return "Top Rear Right";
	case BottomCenter: return "Bottom Center";
	case BottomLeftCenter: return "Bottom Left Center";
	case BottomRightCenter: return "Bottom Right Center";
	default:
		if (value >= AuxRangeStart && value <= AuxRangeEnd) {
			return QString("Aux %1").arg(value - AuxRangeStart + 1);
		} else if (value >= CustomRangeStart) {
			return QString("Custom %1").arg(value - CustomRangeStart + 1);
		} else {
			return "Unknown";
		}
	}
}

void PwNode::bindHooks() {
	// Bind the device first as pw is in order, meaning the device should be bound before
	// we want to do anything with it.
	if (this->device) this->device->ref();

	pw_node_add_listener(this->proxy(), &this->listener.hook, &PwNode::EVENTS, this);
}

void PwNode::unbindHooks() {
	this->listener.remove();
	this->routeDevice = -1;
	this->properties.clear();
	emit this->propertiesChanged();

	if (this->boundData != nullptr) {
		this->boundData->onUnbind();
	}

	// unbind after the node is unbound
	if (this->device) this->device->unref();
}

void PwNode::initProps(const spa_dict* props) {
	if (const auto* mediaClass = spa_dict_lookup(props, SPA_KEY_MEDIA_CLASS)) {
		if (strcmp(mediaClass, "Audio/Sink") == 0) {
			this->type = PwNodeType::Audio;
			this->isSink = true;
			this->isStream = false;
		} else if (strcmp(mediaClass, "Audio/Source") == 0) {
			this->type = PwNodeType::Audio;
			this->isSink = false;
			this->isStream = false;
		} else if (strcmp(mediaClass, "Stream/Output/Audio") == 0) {
			this->type = PwNodeType::Audio;
			this->isSink = false;
			this->isStream = true;
		} else if (strcmp(mediaClass, "Stream/Input/Audio") == 0) {
			this->type = PwNodeType::Audio;
			this->isSink = true;
			this->isStream = true;
		}
	}

	if (const auto* nodeName = spa_dict_lookup(props, SPA_KEY_NODE_NAME)) {
		this->name = nodeName;
	}

	if (const auto* nodeDesc = spa_dict_lookup(props, SPA_KEY_NODE_DESCRIPTION)) {
		this->description = nodeDesc;
	}

	if (const auto* nodeNick = spa_dict_lookup(props, PW_KEY_NODE_NICK)) {
		this->nick = nodeNick;
	}

	if (const auto* deviceId = spa_dict_lookup(props, PW_KEY_DEVICE_ID)) {
		auto ok = false;
		auto id = QString::fromUtf8(deviceId).toInt(&ok);

		if (!ok) {
			qCCritical(logNode) << this << "has a device.id property but the value is not an integer. Id:"
			                    << deviceId;
		} else {
			this->device = this->registry->devices.value(id);

			if (this->device == nullptr) {
				qCCritical(logNode
				) << this
				  << "has a device.id property that does not corrospond to a device object. Id:" << id;
			}
		}
	}

	if (this->type == PwNodeType::Audio) {
		this->boundData = new PwNodeBoundAudio(this);
	}
}

const pw_node_events PwNode::EVENTS = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = &PwNode::onInfo,
    .param = &PwNode::onParam,
};

void PwNode::onInfo(void* data, const pw_node_info* info) {
	auto* self = static_cast<PwNode*>(data);

	if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0) {
		auto properties = QMap<QString, QString>();

		if (self->device) {
			if (const auto* routeDevice = spa_dict_lookup(info->props, "card.profile.device")) {
				auto ok = false;
				auto id = QString::fromUtf8(routeDevice).toInt(&ok);

				if (!ok) {
					qCCritical(logNode
					) << self
					  << "has a card.profile.device property but the value is not an integer. Value:" << id;
				}

				self->routeDevice = id;
			} else {
				qCCritical(logNode) << self << "has attached device" << self->device
				                    << "but no card.profile.device property.";
			}
		}

		const spa_dict_item* item = nullptr;
		spa_dict_for_each(item, info->props) { properties.insert(item->key, item->value); }

		self->properties = properties;
		emit self->propertiesChanged();
	}

	if (self->boundData != nullptr) {
		self->boundData->onInfo(info);
	}
}

void PwNode::onParam(
    void* data,
    qint32 /*seq*/,
    quint32 id,
    quint32 index,
    quint32 /*next*/,
    const spa_pod* param
) {
	auto* self = static_cast<PwNode*>(data);
	if (self->boundData != nullptr) {
		self->boundData->onSpaParam(id, index, param);
	}
}

void PwNodeBoundAudio::onInfo(const pw_node_info* info) {
	if ((info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) != 0) {
		for (quint32 i = 0; i < info->n_params; i++) {
			auto& param = info->params[i]; // NOLINT

			if (param.id == SPA_PARAM_Props && (param.flags & SPA_PARAM_INFO_READ) != 0) {
				pw_node_enum_params(this->node->proxy(), 0, param.id, 0, UINT32_MAX, nullptr);
			}
		}
	}
}

void PwNodeBoundAudio::onSpaParam(quint32 id, quint32 index, const spa_pod* param) {
	if (id == SPA_PARAM_Props && index == 0) {
		this->updateVolumeFromParam(param);
		this->updateMutedFromParam(param);
	}
}

void PwNodeBoundAudio::updateVolumeFromParam(const spa_pod* param) {
	const auto* volumesProp = spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes);
	const auto* channelsProp = spa_pod_find_prop(param, nullptr, SPA_PROP_channelMap);

	const auto* volumes = reinterpret_cast<const spa_pod_array*>(&volumesProp->value);   // NOLINT
	const auto* channels = reinterpret_cast<const spa_pod_array*>(&channelsProp->value); // NOLINT

	auto volumesVec = QVector<float>();
	auto channelsVec = QVector<PwAudioChannel::Enum>();

	spa_pod* iter = nullptr;
	SPA_POD_ARRAY_FOREACH(volumes, iter) {
		// Cubing behavior found in MPD source, and appears to corrospond to everyone else's measurements correctly.
		auto linear = *reinterpret_cast<float*>(iter); // NOLINT
		auto visual = std::cbrt(linear);
		volumesVec.push_back(visual);
	}

	SPA_POD_ARRAY_FOREACH(channels, iter) {
		channelsVec.push_back(*reinterpret_cast<PwAudioChannel::Enum*>(iter)); // NOLINT
	}

	if (volumesVec.size() != channelsVec.size()) {
		qCWarning(logNode) << "Cannot update volume props of" << this->node
		                   << "- channelVolumes and channelMap are not the same size. Sizes:"
		                   << volumesVec.size() << channelsVec.size();
		return;
	}

	// It is important that the lengths of channels and volumes stay in sync whenever you read them.
	auto channelsChanged = false;
	auto volumesChanged = false;

	if (this->mChannels != channelsVec) {
		this->mChannels = channelsVec;
		channelsChanged = true;
		qCInfo(logNode) << "Got updated channels of" << this->node << '-' << this->mChannels;
	}

	if (this->mVolumes != volumesVec) {
		this->mVolumes = volumesVec;
		volumesChanged = true;
		qCInfo(logNode) << "Got updated volumes of" << this->node << '-' << this->mVolumes;
	}

	if (channelsChanged) emit this->channelsChanged();
	if (volumesChanged) emit this->volumesChanged();
}

void PwNodeBoundAudio::updateMutedFromParam(const spa_pod* param) {
	auto parser = spa_pod_parser();
	spa_pod_parser_pod(&parser, param);

	auto muted = false;

	// clang-format off
	quint32 id = SPA_PARAM_Props;
	spa_pod_parser_get_object(
			&parser, SPA_TYPE_OBJECT_Props, &id,
			SPA_PROP_mute, SPA_POD_Bool(&muted)
	);
	// clang-format on

	if (muted != this->mMuted) {
		qCInfo(logNode) << "Got updated mute status of" << this->node << '-' << muted;
		this->mMuted = muted;
		emit this->mutedChanged();
	}
}

void PwNodeBoundAudio::onUnbind() {
	this->mChannels.clear();
	this->mVolumes.clear();
	emit this->channelsChanged();
	emit this->volumesChanged();
}

bool PwNodeBoundAudio::isMuted() const { return this->mMuted; }

void PwNodeBoundAudio::setMuted(bool muted) {
	if (this->node->proxy() == nullptr) {
		qCCritical(logNode) << "Tried to change mute state for" << this->node << "which is not bound.";
		return;
	}

	if (muted == this->mMuted) return;

	if (this->node->device) {
		if (!this->node->device->setMuted(this->node->routeDevice, muted)) {
			return;
		}

		qCInfo(logNode) << "Changed muted state of" << this->node << "to" << muted << "via device";
	} else {
		auto buffer = std::array<quint8, 1024>();
		auto builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());

		// is this a leak? seems like probably not? docs don't say, as usual.
		// clang-format off
		auto* pod = spa_pod_builder_add_object(
				&builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
				SPA_PROP_mute, SPA_POD_Bool(muted)
		);
		// clang-format on

		qCInfo(logNode) << "Changed muted state of" << this->node << "to" << muted;
		pw_node_set_param(this->node->proxy(), SPA_PARAM_Props, 0, static_cast<spa_pod*>(pod));
	}

	this->mMuted = muted;
	emit this->mutedChanged();
}

float PwNodeBoundAudio::averageVolume() const {
	float total = 0;

	for (auto volume: this->mVolumes) {
		total += volume;
	}

	return total / static_cast<float>(this->mVolumes.size());
}

void PwNodeBoundAudio::setAverageVolume(float volume) {
	auto oldAverage = this->averageVolume();
	auto mul = oldAverage == 0 ? 0 : volume / oldAverage;
	auto volumes = QVector<float>();

	for (auto oldVolume: this->mVolumes) {
		volumes.push_back(mul == 0 ? volume : oldVolume * mul);
	}

	this->setVolumes(volumes);
}

QVector<PwAudioChannel::Enum> PwNodeBoundAudio::channels() const { return this->mChannels; }

QVector<float> PwNodeBoundAudio::volumes() const { return this->mVolumes; }

void PwNodeBoundAudio::setVolumes(const QVector<float>& volumes) {
	if (this->node->proxy() == nullptr) {
		qCCritical(logNode) << "Tried to change node volumes for" << this->node
		                    << "which is not bound.";
		return;
	}

	if (volumes == this->mVolumes) return;

	if (volumes.length() != this->mVolumes.length()) {
		qCCritical(logNode) << "Tried to change node volumes for" << this->node << "from"
		                    << this->mVolumes << "to" << volumes
		                    << "which has a different length than the list of channels"
		                    << this->mChannels;
		return;
	}

	if (this->node->device) {
		if (!this->node->device->setVolumes(this->node->routeDevice, volumes)) {
			return;
		}

		qCInfo(logNode) << "Changed volumes of" << this->node << "to" << volumes << "via device";
	} else {
		auto buffer = std::array<quint8, 1024>();
		auto builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());

		auto cubedVolumes = QVector<float>();
		for (auto volume: volumes) {
			cubedVolumes.push_back(volume * volume * volume);
		}

		// clang-format off
		auto* pod = spa_pod_builder_add_object(
				&builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
				SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, cubedVolumes.length(), cubedVolumes.data())
		);
		// clang-format on

		qCInfo(logNode) << "Changed volumes of" << this->node << "to" << volumes;
		pw_node_set_param(this->node->proxy(), SPA_PARAM_Props, 0, static_cast<spa_pod*>(pod));
	}

	this->mVolumes = volumes;
	emit this->volumesChanged();
}

} // namespace qs::service::pipewire
