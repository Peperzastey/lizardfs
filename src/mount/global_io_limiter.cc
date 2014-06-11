#include "config.h"
#include "mount/global_io_limiter.h"

#include <unistd.h>
#include <algorithm>

#include "common/cltoma_communication.h"
#include "common/matocl_communication.h"
#include "common/token_bucket.h"
#include "mount/io_limit_group.h"

using namespace ioLimiting;

MasterLimiter::MasterLimiter() : iolimitsConfigHandler_(*this), configVersion_(0) {
	sassert(fs_register_packet_type_handler(LIZ_MATOCL_IOLIMITS_CONFIG, &iolimitsConfigHandler_));
}

MasterLimiter::~MasterLimiter() {
	sassert(fs_unregister_packet_type_handler(
			LIZ_MATOCL_IOLIMITS_CONFIG, &iolimitsConfigHandler_));
}

uint64_t MasterLimiter::request(const IoLimitGroupId& groupId, uint64_t size) {
	MessageBuffer buffer;
	cltoma::iolimit::serialize(buffer, 0, configVersion_, groupId, size);
	uint8_t status = fs_raw_sendandreceive(buffer, LIZ_MATOCL_IOLIMIT);
	if (status != STATUS_OK) {
		syslog(LOG_NOTICE, "Sending IOLIMIT returned status %s", mfsstrerr(status));
		return 0;
	}
	uint32_t receivedMsgid, receivedConfigVersion;
	std::string receivedGroupId;
	uint64_t receivedSize;
	matocl::iolimit::deserialize(buffer, receivedMsgid, receivedConfigVersion, receivedGroupId,
			receivedSize);
	if (receivedConfigVersion != configVersion_) {
		syslog(LOG_NOTICE,
				"Received unexpected IOLIMIT config version %" PRIu32 " instead of %" PRIu32,
				receivedConfigVersion, configVersion_);
		return 0;
	}
	if (receivedGroupId != groupId) {
		syslog(LOG_NOTICE, "Received IOLIMIT group %s instead of %s",
				receivedGroupId.c_str(), groupId.c_str());
		return 0;
	}
	return receivedSize;
}

bool MasterLimiter::IolimitsConfigHandler::handle(MessageBuffer buffer) {
	try {
		uint32_t configVersion;
		uint32_t delta_us;
		std::string subsystem;
		std::vector<std::string> groups;
		matocl::iolimitsConfig::deserialize(buffer.data(), buffer.size(),
				configVersion, delta_us, subsystem, groups);
		parent_.configVersion_ = configVersion;
		parent_.reconfigure_(delta_us, subsystem, groups);
		syslog(LOG_INFO, "Received IO limits configuration update from master");
		return true;
	} catch (IncorrectDeserializationException& ex) {
		syslog(LOG_ERR, "Malformed MATOCL_IOLIMITS_CONFIG: %s", ex.what());
		return false;
	}
}

uint64_t MountLimiter::request(const IoLimitGroupId& groupId, uint64_t size) {
	return database_.request(SteadyClock::now(), groupId, size);
}

void MountLimiter::loadConfiguration(const IoLimitsConfigLoader& config) {
	database_.setLimits(SteadyClock::now(), config.limits(), 200);
	reconfigure_(1000, config.subsystem(), database_.getGroups());
}

std::shared_ptr<Group> LimiterProxy::getGroup(const IoLimitGroupId& groupId) const {
	Groups::const_iterator groupIt = groups_.find(groupId);
	if (groupIt == groups_.end()) {
		groupIt = groups_.find(kUnclassified);
	}
	if (groupIt == groups_.end()) {
		return nullptr;
	}
	return groupIt->second;
}

uint8_t LimiterProxy::waitForRead(const pid_t pid, const uint64_t size, SteadyTimePoint deadline) {
	std::unique_lock<std::mutex> lock(mutex_);
	uint8_t status;
	do {
		if (!enabled_) {
			return STATUS_OK;
		}
		IoLimitGroupId groupId = getIoLimitGroupIdNoExcept(pid, subsystem_);
		// Grab a shared_ptr reference on the group descriptor so that reconfigure() can
		// quickly unreference this group from the groups_ map without waiting for us.
		std::shared_ptr<Group> group = getGroup(groupId);
		if (!group) {
			return EPERM;
		}
		status = group->wait(size, deadline, lock);
	} while (status == ENOENT); // loop if the group disappeared due to reconfiguration
	return status;
}

uint8_t LimiterProxy::waitForWrite(const pid_t pid, const uint64_t size, SteadyTimePoint deadline) {
	return waitForRead(pid, size, deadline);
}

void LimiterProxy::reconfigure(uint32_t delta_us, const std::string& subsystem,
		const std::vector<IoLimitGroupId>& groupIds) {

	std::vector<std::reference_wrapper<const IoLimitGroupId>>
		newGroupIds(groupIds.begin(), groupIds.end());
	std::sort(newGroupIds.begin(), newGroupIds.end(), std::less<std::string>());

	std::unique_lock<std::mutex> lock(mutex_);

	const bool differentSubsystem = (subsystem_ != subsystem);
	auto newIter = newGroupIds.begin();
	auto oldIter = groups_.begin();

	while (true) {
		while (oldIter != groups_.end() &&
				(newIter == newGroupIds.end() ||
				 newIter->get() > oldIter->first)) {
			// no group with such name in new configuration
			// notify waitees that there's nothing to wait for
			oldIter->second->die();
			// make the group unreachable through groups_
			oldIter = groups_.erase(oldIter);
			// the last waitee's shared_ptr will free the memory
		}
		if (newIter == newGroupIds.end()) {
			// stale groups removed, no more new groups, we are done
			break;
		}
		if (oldIter == groups_.end() ||
				oldIter->first > newIter->get()) {
			// new group has been added
			const std::string& groupId = newIter->get();
			oldIter = groups_.insert(oldIter, std::move(std::make_pair(groupId,
					std::move(std::make_shared<Group>(shared_, groupId, clock_)))));
		} else {
			// existing group with the same name
			if (differentSubsystem) {
				// now it isn't the same group anymore
				// notify waitees
				oldIter->second->die();
				// unreference the old group and create a new one
				const std::string& groupId = newIter->get();
				oldIter->second = std::move(std::make_shared<Group>(shared_, groupId, clock_));
			}
		}
		newIter++;
		oldIter++;
	}
	shared_.delta = std::chrono::microseconds(delta_us);
	subsystem_ = subsystem;
	enabled_ = (subsystem_ != "" || groups_.count(kUnclassified) == 1);
}
