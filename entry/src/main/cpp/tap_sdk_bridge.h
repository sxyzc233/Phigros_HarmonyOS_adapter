#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct ReaSdkRequest { uint32_t id; std::string command; };
// Raw payloads stay in memory. Never print credentials or identity data.
bool ReaSdkEnqueue(const std::string &command, void *proxy);
std::vector<ReaSdkRequest> ReaSdkTakeRequests();
bool ReaSdkReply(uint32_t id, const std::string &result, bool finished);
void ReaSdkRelease(uint32_t id);
void ReaSdkDrainReplies(); // Unity host thread only.
extern "C" bool ReaDeliverSdkCallback(void *proxy, const char *result);
