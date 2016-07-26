/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2013, Ioan A. Sucan
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Ioan A. Sucan nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Ioan Sucan, Dave Coleman, Robert Haschke */

#include "moveit_fake_controllers.h"
#include <ros/ros.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit/robot_model_loader/robot_model_loader.h>
#include <sensor_msgs/JointState.h>
#include <map>
#include <iterator>

namespace moveit_fake_controller_manager
{
static const std::string DEFAULT_TYPE = "interpolation";
static const std::string ROBOT_DESCRIPTION = "robot_description";

class MoveItFakeControllerManager : public moveit_controller_manager::MoveItControllerManager
{
public:
  MoveItFakeControllerManager() : node_handle_("~")
  {
    if (!node_handle_.hasParam("controller_list"))
    {
      ROS_ERROR_STREAM("MoveItFakeControllerManager: No controller_list specified.");
      return;
    }

    XmlRpc::XmlRpcValue controller_list;
    node_handle_.getParam("controller_list", controller_list);
    if (controller_list.getType() != XmlRpc::XmlRpcValue::TypeArray)
    {
      ROS_ERROR("MoveItFakeControllerManager: controller_list should be specified as an array");
      return;
    }

    pub_ = node_handle_.advertise<sensor_msgs::JointState>("fake_controller_joint_states", 100, false);

    /* publish initial pose */
    XmlRpc::XmlRpcValue initial;
    if (node_handle_.getParam("initial", initial))
    {
      sensor_msgs::JointState js = loadInitialJointValues(initial);
      js.header.stamp = ros::Time::now();
      pub_.publish(js);
    }

    /* actually create each controller */
    for (int i = 0; i < controller_list.size(); ++i)
    {
      if (!controller_list[i].hasMember("name") || !controller_list[i].hasMember("joints"))
      {
        ROS_ERROR("MoveItFakeControllerManager: Name and joints must be specifed for each controller");
        continue;
      }

      try
      {
        std::string name = std::string(controller_list[i]["name"]);

        if (controller_list[i]["joints"].getType() != XmlRpc::XmlRpcValue::TypeArray)
        {
          ROS_ERROR_STREAM("MoveItFakeControllerManager: The list of joints for controller "
                           << name << " is not specified as an array");
          continue;
        }
        std::vector<std::string> joints;
        for (int j = 0; j < controller_list[i]["joints"].size(); ++j)
          joints.push_back(std::string(controller_list[i]["joints"][j]));

        const std::string &type =
            controller_list[i].hasMember("type") ? std::string(controller_list[i]["type"]) : DEFAULT_TYPE;
        if (type == "last point")
          controllers_[name].reset(new LastPointController(name, joints, pub_));
        else if (type == "via points")
          controllers_[name].reset(new ViaPointController(name, joints, pub_));
        else if (type == "interpolate")
          controllers_[name].reset(new InterpolatingController(name, joints, pub_));
        else
          ROS_ERROR_STREAM("Unknown fake controller type: " << type);
      }
      catch (...)
      {
        ROS_ERROR("MoveItFakeControllerManager: Unable to parse controller information");
      }
    }
  }

  sensor_msgs::JointState loadInitialJointValues(XmlRpc::XmlRpcValue& param) const
  {
    sensor_msgs::JointState js;

    if (param.getType() != XmlRpc::XmlRpcValue::TypeArray || param.size() == 0)
    {
      ROS_ERROR_ONCE_NAMED("loadInitialJointValues", "Parameter 'initial' should be an array of (group, pose) structs.");
      return js;
    }

    robot_model_loader::RobotModelLoader robot_model_loader(ROBOT_DESCRIPTION);
    robot_model::RobotModelPtr robot_model = robot_model_loader.getModel();
    typedef std::map<std::string, double> JointPoseMap;
    JointPoseMap joints;

    for (int i = 0, end = param.size(); i != end; ++i)
    {
      try
      {
        std::string group_name = std::string(param[i]["group"]);
        std::string pose_name = std::string(param[i]["pose"]);
        if (!robot_model->hasJointModelGroup(group_name))
        {
          ROS_WARN_STREAM_NAMED("loadInitialJointValues", "Unknown joint model group: " << group_name);
          continue;
        }
        moveit::core::JointModelGroup* jmg = robot_model->getJointModelGroup(group_name);
        moveit::core::RobotState robot_state(robot_model);
        const std::vector<std::string> &joint_names = jmg->getActiveJointModelNames();

        if (!robot_state.setToDefaultValues(jmg, pose_name))
        {
          ROS_WARN_NAMED("loadInitialJointValues", "Unknown pose '%s' for group '%s'.", pose_name.c_str(), group_name.c_str());
          continue;
        }
        ROS_INFO_NAMED("loadInitialJointValues", "Set joints of group '%s' to pose '%s'.", group_name.c_str(), pose_name.c_str());

        for (std::vector<std::string>::const_iterator jit = joint_names.begin(), end = joint_names.end(); jit != end; ++jit)
        {
          const moveit::core::JointModel* jm = robot_state.getJointModel(*jit);
          if (!jm)
          {
            ROS_WARN_STREAM_NAMED("loadInitialJointValues", "Unknown joint: " << *jit);
            continue;
          }
          if (jm->getVariableCount() != 1)
          {
            ROS_WARN_STREAM_NAMED("loadInitialJointValues", "Cannot handle multi-variable joint: " << *jit);
            continue;
          }

          joints[*jit] = robot_state.getJointPositions(jm)[0];
        }
      }
      catch (...)
      {
        ROS_ERROR_ONCE_NAMED("loadInitialJointValues", "Unable to parse initial pose information.");
      }
    }

    // fill the joint state
    for (JointPoseMap::const_iterator it = joints.begin(), end = joints.end(); it != end; ++it)
    {
      js.name.push_back(it->first);
      js.position.push_back(it->second);
    }
    return js;
  }

  virtual ~MoveItFakeControllerManager()
  {
  }

  /*
   * Get a controller, by controller name (which was specified in the controllers.yaml
   */
  virtual moveit_controller_manager::MoveItControllerHandlePtr getControllerHandle(const std::string &name)
  {
    std::map<std::string, BaseFakeControllerPtr>::const_iterator it = controllers_.find(name);
    if (it != controllers_.end())
      return it->second;
    else
      ROS_FATAL_STREAM("No such controller: " << name);
    return moveit_controller_manager::MoveItControllerHandlePtr();
  }

  /*
   * Get the list of controller names.
   */
  virtual void getControllersList(std::vector<std::string> &names)
  {
    for (std::map<std::string, BaseFakeControllerPtr>::const_iterator it = controllers_.begin();
         it != controllers_.end(); ++it)
      names.push_back(it->first);
    ROS_INFO_STREAM("Returned " << names.size() << " controllers in list");
  }

  /*
   * Fake controllers are always active
   */
  virtual void getActiveControllers(std::vector<std::string> &names)
  {
    getControllersList(names);
  }

  /*
   * Fake controllers are always loaded
   */
  virtual void getLoadedControllers(std::vector<std::string> &names)
  {
    getControllersList(names);
  }

  /*
   * Get the list of joints that a controller can control.
   */
  virtual void getControllerJoints(const std::string &name, std::vector<std::string> &joints)
  {
    std::map<std::string, BaseFakeControllerPtr>::const_iterator it = controllers_.find(name);
    if (it != controllers_.end())
    {
      it->second->getJoints(joints);
    }
    else
    {
      ROS_WARN("The joints for controller '%s' are not known. Perhaps the controller configuration is not loaded on "
               "the param server?", name.c_str());
      joints.clear();
    }
  }

  /*
   * Controllers are all active and default.
   */
  virtual moveit_controller_manager::MoveItControllerManager::ControllerState
  getControllerState(const std::string &name)
  {
    moveit_controller_manager::MoveItControllerManager::ControllerState state;
    state.active_ = true;
    state.default_ = true;
    return state;
  }

  /* Cannot switch our controllers */
  virtual bool switchControllers(const std::vector<std::string> &activate, const std::vector<std::string> &deactivate)
  {
    return false;
  }

protected:
  ros::NodeHandle node_handle_;
  ros::Publisher pub_;
  std::map<std::string, BaseFakeControllerPtr> controllers_;
};

}  // end namespace moveit_fake_controller_manager

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(moveit_fake_controller_manager::MoveItFakeControllerManager,
                       moveit_controller_manager::MoveItControllerManager);
