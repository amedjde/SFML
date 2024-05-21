////////////////////////////////////////////////////////////
//
// SFML - Simple and Fast Multimedia Library
// Copyright (C) 2007-2024 Laurent Gomila (laurent@sfml-dev.org)
//
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it freely,
// subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented;
//    you must not claim that you wrote the original software.
//    If you use this software in a product, an acknowledgment
//    in the product documentation would be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such,
//    and must not be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//
////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////
// Headers
////////////////////////////////////////////////////////////
#include <SFML/Audio/AudioDevice.hpp>
#include <SFML/Audio/PlaybackDevice.hpp>

#include <SFML/System/Err.hpp>

#include <algorithm>
#include <array>
#include <ostream>
#include <unordered_map>

#include <cassert>


namespace sf::priv
{
////////////////////////////////////////////////////////////
AudioDevice::AudioDevice()
{
    // Ensure we only ever have a single AudioDevice instance
    assert(getInstance() == nullptr);
    getInstance() = this;

    // Create the log
    m_log.emplace();

    if (const auto result = ma_log_init(nullptr, &*m_log); result != MA_SUCCESS)
    {
        m_log.reset();
        err() << "Failed to initialize the audio log: " << ma_result_description(result) << std::endl;
        return;
    }

    // Register our logging callback to output any warning/error messages
    if (const auto result = ma_log_register_callback(&*m_log,
                                                     ma_log_callback_init(
                                                         [](void*, ma_uint32 level, const char* message)
                                                         {
                                                             if (level <= MA_LOG_LEVEL_WARNING)
                                                                 err() << "miniaudio " << ma_log_level_to_string(level)
                                                                       << ": " << message << std::flush;
                                                         },
                                                         nullptr));
        result != MA_SUCCESS)
        err() << "Failed to register audio log callback: " << ma_result_description(result) << std::endl;

    // Create the context
    m_context.emplace();

    auto contextConfig                                 = ma_context_config_init();
    contextConfig.pLog                                 = &*m_log;
    ma_uint32                              deviceCount = 0;
    const auto                             nullBackend = ma_backend_null;
    const std::array<const ma_backend*, 2> backendLists{nullptr, &nullBackend};

    for (const auto* backendList : backendLists)
    {
        // We can set backendCount to 1 since it is ignored when backends is set to nullptr
        if (const auto result = ma_context_init(backendList, 1, &contextConfig, &*m_context); result != MA_SUCCESS)
        {
            m_context.reset();
            err() << "Failed to initialize the audio playback context: " << ma_result_description(result) << std::endl;
            return;
        }

        // Count the playback devices
        if (const auto result = ma_context_get_devices(&*m_context, nullptr, &deviceCount, nullptr, nullptr);
            result != MA_SUCCESS)
        {
            err() << "Failed to get audio playback devices: " << ma_result_description(result) << std::endl;
            return;
        }

        // Check if there are audio playback devices available on the system
        if (deviceCount > 0)
            break;

        // Warn if no devices were found using the default backend list
        if (backendList == nullptr)
            err() << "No audio playback devices available on the system" << std::endl;

        // Clean up the context if we didn't find any devices
        ma_context_uninit(&*m_context);
    }

    // If the NULL audio backend also doesn't provide a device we give up
    if (deviceCount == 0)
    {
        m_context.reset();
        return;
    }

    if (m_context->backend == ma_backend_null)
        err() << "Using NULL audio backend for playback" << std::endl;

    initialize();
}


////////////////////////////////////////////////////////////
AudioDevice::~AudioDevice()
{
    // Destroy the engine
    if (m_engine)
        ma_engine_uninit(&*m_engine);

    // Destroy the playback device
    if (m_playbackDevice)
        ma_device_uninit(&*m_playbackDevice);

    // Destroy the context
    if (m_context)
        ma_context_uninit(&*m_context);

    // Destroy the log
    if (m_log)
        ma_log_uninit(&*m_log);

    // Ensure we only ever have a single AudioDevice instance
    assert(getInstance() != nullptr);
    getInstance() = nullptr;
}


////////////////////////////////////////////////////////////
ma_engine* AudioDevice::getEngine()
{
    auto* instance = getInstance();

    if (instance && instance->m_engine)
        return &*instance->m_engine;

    return nullptr;
}


////////////////////////////////////////////////////////////
bool AudioDevice::reinitialize()
{
    auto* instance = getInstance();

    // We don't have to do anything if an instance doesn't exist yet
    if (!instance)
        return true;

    const std::lock_guard lock(instance->m_resourcesMutex);

    // Deinitialize all audio resources
    for (const auto& entry : instance->m_resources)
        entry.deinitializeFunc(entry.resource);

    // Destroy the old engine
    if (instance->m_engine)
        ma_engine_uninit(&*instance->m_engine);

    // Destroy the old playback device
    if (instance->m_playbackDevice)
        ma_device_uninit(&*instance->m_playbackDevice);

    // Create the new objects
    const auto result = instance->initialize();

    // Reinitialize all audio resources
    for (const auto& entry : instance->m_resources)
        entry.reinitializeFunc(entry.resource);

    return result;
}


////////////////////////////////////////////////////////////
std::vector<AudioDevice::DeviceEntry> AudioDevice::getAvailableDevices()
{
    auto getDevices = [](auto& context)
    {
        ma_device_info* deviceInfos{};
        ma_uint32       deviceCount{};

        // Get the playback devices
        if (const auto result = ma_context_get_devices(&context, &deviceInfos, &deviceCount, nullptr, nullptr);
            result != MA_SUCCESS)
        {
            ma_context_uninit(&context);
            err() << "Failed to get audio playback devices: " << ma_result_description(result) << std::endl;
            return std::vector<DeviceEntry>{};
        }

        std::vector<DeviceEntry> deviceList;
        deviceList.reserve(deviceCount);

        // In order to report devices with identical names and still allow
        // the user to differentiate between them when selecting, we append
        // an index (number) to their name starting from the second entry
        std::unordered_map<std::string, int> deviceIndices;
        deviceIndices.reserve(deviceCount);

        for (auto i = 0u; i < deviceCount; ++i)
        {
            auto  name  = std::string{deviceInfos[i].name};
            auto& index = deviceIndices[name];

            ++index;

            if (index > 1)
                name += ' ' + std::to_string(index);

            // Make sure the default device is always placed at the front
            deviceList.emplace(deviceInfos[i].isDefault ? deviceList.begin() : deviceList.end(),
                               DeviceEntry{name, deviceInfos[i].id, deviceInfos[i].isDefault == MA_TRUE});
        }

        return deviceList;
    };

    // Use an existing instance's context if one exists
    auto* instance = getInstance();

    if (instance && instance->m_context)
        return getDevices(*instance->m_context);

    // Otherwise, construct a temporary context
    ma_context context{};

    if (const auto result = ma_context_init(nullptr, 0, nullptr, &context); result != MA_SUCCESS)
    {
        err() << "Failed to initialize the audio playback context: " << ma_result_description(result) << std::endl;
        return {};
    }

    auto deviceList = getDevices(context);
    ma_context_uninit(&context);
    return deviceList;
}


////////////////////////////////////////////////////////////
AudioDevice::ResourceEntryIter AudioDevice::registerResource(void*               resource,
                                                             ResourceEntry::Func deinitializeFunc,
                                                             ResourceEntry::Func reinitializeFunc)
{
    // There should always be an AudioDevice instance when registerResource is called
    auto* instance = getInstance();
    assert(instance && "AudioDevice instance should exist when calling AudioDevice::registerResource");
    const std::lock_guard lock(instance->m_resourcesMutex);
    return instance->m_resources.insert(instance->m_resources.end(), {resource, deinitializeFunc, reinitializeFunc});
}


////////////////////////////////////////////////////////////
void AudioDevice::unregisterResource(AudioDevice::ResourceEntryIter resourceEntry)
{
    // There should always be an AudioDevice instance when unregisterResource is called
    auto* instance = getInstance();
    assert(instance && "AudioDevice instance should exist when calling AudioDevice::unregisterResource");
    const std::lock_guard lock(instance->m_resourcesMutex);
    instance->m_resources.erase(resourceEntry);
}


////////////////////////////////////////////////////////////
void AudioDevice::setGlobalVolume(float volume)
{
    // Store the volume in case no audio device exists yet
    getListenerProperties().volume = volume;

    auto* instance = getInstance();

    if (!instance || !instance->m_engine)
        return;

    if (const auto result = ma_device_set_master_volume(ma_engine_get_device(&*instance->m_engine), volume * 0.01f);
        result != MA_SUCCESS)
        err() << "Failed to set audio device master volume: " << ma_result_description(result) << std::endl;
}


////////////////////////////////////////////////////////////
float AudioDevice::getGlobalVolume()
{
    return getListenerProperties().volume;
}


////////////////////////////////////////////////////////////
void AudioDevice::setPosition(const Vector3f& position)
{
    // Store the position in case no audio device exists yet
    getListenerProperties().position = position;

    auto* instance = getInstance();

    if (!instance || !instance->m_engine)
        return;

    ma_engine_listener_set_position(&*instance->m_engine, 0, position.x, position.y, position.z);
}


////////////////////////////////////////////////////////////
Vector3f AudioDevice::getPosition()
{
    return getListenerProperties().position;
}


////////////////////////////////////////////////////////////
void AudioDevice::setDirection(const Vector3f& direction)
{
    // Store the direction in case no audio device exists yet
    getListenerProperties().direction = direction;

    auto* instance = getInstance();

    if (!instance || !instance->m_engine)
        return;

    ma_engine_listener_set_direction(&*instance->m_engine, 0, direction.x, direction.y, direction.z);
}


////////////////////////////////////////////////////////////
Vector3f AudioDevice::getDirection()
{
    return getListenerProperties().direction;
}


////////////////////////////////////////////////////////////
void AudioDevice::setVelocity(const Vector3f& velocity)
{
    // Store the velocity in case no audio device exists yet
    getListenerProperties().velocity = velocity;

    auto* instance = getInstance();

    if (!instance || !instance->m_engine)
        return;

    ma_engine_listener_set_velocity(&*instance->m_engine, 0, velocity.x, velocity.y, velocity.z);
}


////////////////////////////////////////////////////////////
Vector3f AudioDevice::getVelocity()
{
    return getListenerProperties().velocity;
}


////////////////////////////////////////////////////////////
void AudioDevice::setCone(const Listener::Cone& cone)
{
    // Store the cone in case no audio device exists yet
    getListenerProperties().cone = cone;

    auto* instance = getInstance();

    if (!instance || !instance->m_engine)
        return;

    ma_engine_listener_set_cone(&*instance->m_engine,
                                0,
                                std::clamp(cone.innerAngle, Angle::Zero, degrees(360.f)).asRadians(),
                                std::clamp(cone.outerAngle, Angle::Zero, degrees(360.f)).asRadians(),
                                cone.outerGain);
}


////////////////////////////////////////////////////////////
Listener::Cone AudioDevice::getCone()
{
    return getListenerProperties().cone;
}


////////////////////////////////////////////////////////////
void AudioDevice::setUpVector(const Vector3f& upVector)
{
    // Store the up vector in case no audio device exists yet
    getListenerProperties().upVector = upVector;

    auto* instance = getInstance();

    if (!instance || !instance->m_engine)
        return;

    ma_engine_listener_set_world_up(&*instance->m_engine, 0, upVector.x, upVector.y, upVector.z);
}


////////////////////////////////////////////////////////////
Vector3f AudioDevice::getUpVector()
{
    return getListenerProperties().upVector;
}


////////////////////////////////////////////////////////////
std::optional<ma_device_id> AudioDevice::getSelectedDeviceId() const
{
    const auto devices    = getAvailableDevices();
    const auto deviceName = PlaybackDevice::getDevice();

    auto iter = std::find_if(devices.begin(),
                             devices.end(),
                             [&](const auto& device) { return device.name == deviceName; });

    if (iter != devices.end())
        return iter->id;

    return std::nullopt;
}


////////////////////////////////////////////////////////////
bool AudioDevice::initialize()
{
    const auto deviceId = getSelectedDeviceId();

    // Create the playback device
    m_playbackDevice.emplace();

    auto playbackDeviceConfig         = ma_device_config_init(ma_device_type_playback);
    playbackDeviceConfig.dataCallback = [](ma_device* device, void* output, const void*, ma_uint32 frameCount)
    {
        auto& audioDevice = *static_cast<AudioDevice*>(device->pUserData);

        if (audioDevice.m_engine)
        {
            if (const auto result = ma_engine_read_pcm_frames(&*audioDevice.m_engine, output, frameCount, nullptr);
                result != MA_SUCCESS)
                err() << "Failed to read PCM frames from audio engine: " << ma_result_description(result) << std::endl;
        }
    };
    playbackDeviceConfig.pUserData          = this;
    playbackDeviceConfig.playback.format    = ma_format_f32;
    playbackDeviceConfig.playback.pDeviceID = deviceId ? &*deviceId : nullptr;

    if (const auto result = ma_device_init(&*m_context, &playbackDeviceConfig, &*m_playbackDevice); result != MA_SUCCESS)
    {
        m_playbackDevice.reset();
        err() << "Failed to initialize the audio playback device: " << ma_result_description(result) << std::endl;
        return false;
    }

    // Create the engine
    auto engineConfig          = ma_engine_config_init();
    engineConfig.pContext      = &*m_context;
    engineConfig.pDevice       = &*m_playbackDevice;
    engineConfig.listenerCount = 1;

    m_engine.emplace();

    if (const auto result = ma_engine_init(&engineConfig, &*m_engine); result != MA_SUCCESS)
    {
        m_engine.reset();
        err() << "Failed to initialize the audio engine: " << ma_result_description(result) << std::endl;
        return false;
    }

    // Set master volume, position, velocity, cone and world up vector
    if (const auto result = ma_device_set_master_volume(ma_engine_get_device(&*m_engine),
                                                        getListenerProperties().volume * 0.01f);
        result != MA_SUCCESS)
        err() << "Failed to set audio device master volume: " << ma_result_description(result) << std::endl;

    ma_engine_listener_set_position(&*m_engine,
                                    0,
                                    getListenerProperties().position.x,
                                    getListenerProperties().position.y,
                                    getListenerProperties().position.z);
    ma_engine_listener_set_velocity(&*m_engine,
                                    0,
                                    getListenerProperties().velocity.x,
                                    getListenerProperties().velocity.y,
                                    getListenerProperties().velocity.z);
    ma_engine_listener_set_cone(&*m_engine,
                                0,
                                getListenerProperties().cone.innerAngle.asRadians(),
                                getListenerProperties().cone.outerAngle.asRadians(),
                                getListenerProperties().cone.outerGain);
    ma_engine_listener_set_world_up(&*m_engine,
                                    0,
                                    getListenerProperties().upVector.x,
                                    getListenerProperties().upVector.y,
                                    getListenerProperties().upVector.z);

    return true;
}


////////////////////////////////////////////////////////////
AudioDevice*& AudioDevice::getInstance()
{
    static AudioDevice* instance{};
    return instance;
}


////////////////////////////////////////////////////////////
AudioDevice::ListenerProperties& AudioDevice::getListenerProperties()
{
    static ListenerProperties properties;
    return properties;
}

} // namespace sf::priv
