/*
 * Copyright (C) 2018 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <set>

#include <ignition/plugin/Register.hh>

#include <sdf/Sensor.hh>

#include <ignition/common/Profiler.hh>
#include <ignition/common/Time.hh>

#include <ignition/math/Helpers.hh>

#include <ignition/rendering/Scene.hh>
#include <ignition/sensors/RenderingSensor.hh>
#include <ignition/sensors/Manager.hh>

#include "ignition/gazebo/components/Camera.hh"
#include "ignition/gazebo/components/DepthCamera.hh"
#include "ignition/gazebo/components/GpuLidar.hh"
#include "ignition/gazebo/components/RgbdCamera.hh"
#include "ignition/gazebo/Events.hh"
#include "ignition/gazebo/EntityComponentManager.hh"

#include "ignition/gazebo/rendering/RenderUtil.hh"

#include "Sensors.hh"

using namespace ignition;
using namespace gazebo;
using namespace systems;

// Private data class.
class ignition::gazebo::systems::SensorsPrivate
{
  /// \brief Sensor manager object. This manages the lifecycle of the
  /// instantiated sensors.
  public: sensors::Manager sensorManager;

  /// \brief used to store whether rendering objects have been created.
  public: bool initialized = false;

  /// \brief Main rendering interface
  public: RenderUtil renderUtil;

  /// \brief Unique set of senor ids
  public: std::set<sensors::SensorId> sensorIds;

  /// \brief rendering scene to be managed by the scene manager and used to
  /// generate sensor data
  public: rendering::ScenePtr scene;

  public: std::atomic<bool> running { false };

  public: std::thread renderThread;
  public: std::mutex renderMutex;
  public: std::condition_variable renderCv;

  public: bool doInit { false };
  public: bool updateAvailable { false };
  public: ignition::common::ConnectionPtr stopConn;
  public: ignition::common::Time updateTime;
  public: std::vector<sensors::RenderingSensor *> activeSensors;

  public: std::mutex sensorMaskMutex;
  public: std::map<sensors::SensorId, ignition::common::Time> sensorMask;
};

//////////////////////////////////////////////////
Sensors::Sensors() : System(), dataPtr(std::make_unique<SensorsPrivate>())
{
}

//////////////////////////////////////////////////
Sensors::~Sensors()
{
  this->Stop();
}

//////////////////////////////////////////////////
void Sensors::Stop()
{
  igndbg << "Sensors::Stop" << std::endl;
  std::unique_lock<std::mutex> lock(this->dataPtr->renderMutex);
  this->dataPtr->running = false;
  lock.unlock();
  this->dataPtr->renderCv.notify_all();

  if (this->dataPtr->renderThread.joinable())
  {
    this->dataPtr->renderThread.join();
  }
}

//////////////////////////////////////////////////
void Sensors::RunLoop()
{
  igndbg << "Sensors::RunLoop started" << std::endl;
  while(!this->dataPtr->initialized && this->dataPtr->running)
  {
    igndbg << "Waiting for init" << std::endl;
    std::unique_lock<std::mutex> lock(this->dataPtr->renderMutex);
    this->dataPtr->renderCv.wait(lock);

    if(this->dataPtr->doInit)
    {
      // Only initialize if there are rendering sensors
      igndbg << "Initializing render context" << std::endl;
      this->dataPtr->renderUtil.Init();
      this->dataPtr->scene = this->dataPtr->renderUtil.Scene();
      this->dataPtr->initialized = true;
    }

    this->dataPtr->updateAvailable = false;
    this->dataPtr->renderCv.notify_one();
  }

  igndbg << "Rendering Thread initialized" << std::endl;

  while (this->dataPtr->running)
  {

    std::unique_lock<std::mutex> lock(this->dataPtr->renderMutex);
    this->dataPtr->renderCv.wait(lock, [this](){
        return !this->dataPtr->running || this->dataPtr->updateAvailable;
    });

    if (!this->dataPtr->running)
    {
      break;
    }

    IGN_PROFILE("Sensors::RenderLoop");

    {
      IGN_PROFILE("Update");
      this->dataPtr->renderUtil.Update();
    }

    if (!this->dataPtr->activeSensors.empty())
    {
      // Update the scene graph manually to improve performance
      // We only need to do this once per frame It is important to call
      // sensors::RenderingSensor::SetManualSceneUpdate and set it to true
      // so we don't waste cycles doing one scene graph update per sensor

      this->dataPtr->sensorMaskMutex.lock();
      for(const auto & sensor : this->dataPtr->activeSensors)
      {
        ignition::common::Time delta(0.9 / sensor->UpdateRate());
        this->dataPtr->sensorMask[sensor->Id()] = this->dataPtr->updateTime + delta;
        //igndbg << "Masking: " << sensor->Id() << " until " << this->dataPtr->sensorMask[sensor->Id()] << std::endl;
      }
      this->dataPtr->sensorMaskMutex.unlock();

      {
        IGN_PROFILE("PreRender");
        this->dataPtr->scene->PreRender();
      }

      {
        // publish data
        IGN_PROFILE("RunOnce");
        this->dataPtr->sensorManager.RunOnce(this->dataPtr->updateTime);
      }

      this->dataPtr->activeSensors.clear();
    }

    this->dataPtr->updateAvailable = false;

    lock.unlock();
    this->dataPtr->renderCv.notify_one();
  }
  igndbg << "Terminating render loop" << std::endl;
}

//////////////////////////////////////////////////
void Sensors::Configure(const Entity &/*_id*/,
    const std::shared_ptr<const sdf::Element> &_sdf,
    EntityComponentManager &/*_ecm*/,
    EventManager &_eventMgr)
{
  igndbg << "Configuring Sensors system" << std::endl;
  // Setup rendering
  std::string engineName =
      _sdf->Get<std::string>("render_engine", "ogre2").first;

  this->dataPtr->renderUtil.SetEngineName(engineName);
  this->dataPtr->renderUtil.SetEnableSensors(true,
      std::bind(&Sensors::CreateSensor, this,
      std::placeholders::_1, std::placeholders::_2));

  this->dataPtr->stopConn = _eventMgr.Connect<events::Stop>(std::bind(&Sensors::Stop, this));

  this->dataPtr->running = true;
  this->dataPtr->renderThread = std::thread(&Sensors::RunLoop, this);
}

//////////////////////////////////////////////////
void Sensors::PostUpdate(const UpdateInfo &_info,
                         const EntityComponentManager &_ecm)
{
  IGN_PROFILE("Sensors::PostUpdate");

  if (!this->dataPtr->initialized &&
      (_ecm.HasComponentType(components::Camera::typeId) ||
       _ecm.HasComponentType(components::DepthCamera::typeId) ||
       _ecm.HasComponentType(components::GpuLidar::typeId) ||
       _ecm.HasComponentType(components::RgbdCamera::typeId)))
  {
    igndbg << "Initialization needed" << std::endl;
    std::unique_lock<std::mutex> lock(this->dataPtr->renderMutex);
    this->dataPtr->renderCv.wait(lock, [this] {
        return !this->dataPtr->running || !this->dataPtr->updateAvailable; });
    this->dataPtr->doInit = true;
    lock.unlock();
    this->dataPtr->renderCv.notify_one();
  }


  if (this->dataPtr->running && this->dataPtr->initialized)
  {
    this->dataPtr->renderUtil.UpdateFromECM(_info, _ecm);

    auto time = math::durationToSecNsec(_info.simTime);
    auto t = common::Time(time.first, time.second);

    std::vector<sensors::RenderingSensor *> activeSensors;

    this->dataPtr->sensorMaskMutex.lock();
    for (auto id : this->dataPtr->sensorIds)
    {
      sensors::Sensor *s = this->dataPtr->sensorManager.Sensor(id);
      sensors::RenderingSensor *rs = dynamic_cast<sensors::RenderingSensor *>(s);

      auto it = this->dataPtr->sensorMask.find(id);
      if (it != this->dataPtr->sensorMask.end())

      {
        if (it->second <= t)
        {
          //igndbg << "Unmasking : " << id << " at " << t << std::endl;
          this->dataPtr->sensorMask.erase(it);
        }
        else
        {
          continue;
        }
      }

      if (rs && rs->NextUpdateTime() <= t)
      {
        activeSensors.push_back(rs);
      }
    }
    this->dataPtr->sensorMaskMutex.unlock();

    if (activeSensors.size() || this->dataPtr->renderUtil.PendingSensors() > 0)
    {
      //igndbg << "Update needed: " << activeSensors.size() << " " << this->dataPtr->renderUtil.PendingSensors() <<
      //  t << std::endl;
      std::unique_lock<std::mutex> lock(this->dataPtr->renderMutex);
      this->dataPtr->renderCv.wait(lock, [this] {
        return !this->dataPtr->running || !this->dataPtr->updateAvailable; });

      if (!this->dataPtr->running)
      {
        return;
      }

      this->dataPtr->activeSensors = std::move(activeSensors);
      this->dataPtr->updateTime = t;
      this->dataPtr->updateAvailable = true;
      this->dataPtr->renderCv.notify_one();
    }
  }
}

//////////////////////////////////////////////////
std::string Sensors::CreateSensor(const sdf::Sensor &_sdf,
    const std::string &_parentName)
{
  if (_sdf.Type() == sdf::SensorType::NONE)
  {
    ignerr << "Unable to create sensor. SDF sensor type is NONE." << std::endl;
    return std::string();
  }

  // Create within ign-sensors
  auto sensorId = this->dataPtr->sensorManager.CreateSensor(_sdf);
  auto sensor = this->dataPtr->sensorManager.Sensor(sensorId);
  this->dataPtr->sensorIds.insert(sensorId);

  if (nullptr == sensor || sensors::NO_SENSOR == sensor->Id())
  {
    ignerr << "Failed to create sensor [" << _sdf.Name()
           << "]" << std::endl;
  }

  // Set the scene so it can create the rendering sensor
  auto renderingSensor =
      dynamic_cast<sensors::RenderingSensor *>(sensor);
  renderingSensor->SetScene(this->dataPtr->scene);
  renderingSensor->SetParent(_parentName);
  renderingSensor->SetManualSceneUpdate(true);

  return sensor->Name();
}

IGNITION_ADD_PLUGIN(Sensors, System,
  Sensors::ISystemConfigure,
  Sensors::ISystemPostUpdate
)

IGNITION_ADD_PLUGIN_ALIAS(Sensors, "ignition::gazebo::systems::Sensors")
