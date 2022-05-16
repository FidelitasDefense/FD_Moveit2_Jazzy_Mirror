/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2021, PickNik Robotics
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
 *   * Neither the name of PickNik Robotics nor the names of its
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

/* Author: David V. Lu!! */

#include <moveit_setup_simulation/simulation.hpp>
#include <moveit_setup_simulation/xml_manipulation.hpp>

namespace moveit_setup_simulation
{
void Simulation::onInit()
{
  urdf_config_ = config_data_->get<moveit_setup_framework::URDFConfig>("urdf");
}

// ******************************************************************************************
// Helper function to get the controller that is controlling the joint
// ******************************************************************************************
std::string Simulation::getJointHardwareInterface(const std::string& joint_name)
{
  /** TODO: Need to port this - may require depending on moveit_setup_controllers ControllerConfig

  for (ControllerConfig& ros_control_config : controller_configs_)
  {
    std::vector<std::string>::iterator joint_it =
        std::find(ros_control_config.joints_.begin(), ros_control_config.joints_.end(), joint_name);
    if (joint_it != ros_control_config.joints_.end())
    {
      if (ros_control_config.type_.substr(0, 8) == "position")
        return "hardware_interface/PositionJointInterface";
      else if (ros_control_config.type_.substr(0, 8) == "velocity")
        return "hardware_interface/VelocityJointInterface";
      // As of writing this, available joint command interfaces are position, velocity and effort.
      else
        return "hardware_interface/EffortJointInterface";
    }
  }
  */
  // If the joint was not found in any controller return EffortJointInterface
  return "hardware_interface/EffortJointInterface";
}

// ******************************************************************************************
// Writes a Gazebo compatible robot URDF to gazebo_compatible_urdf_string_
// ******************************************************************************************
std::string Simulation::getGazeboCompatibleURDF()
{
  TiXmlDocument doc;
  std::string urdf_string = urdf_config_->getURDFContents();
  doc.Parse(urdf_string.c_str(), nullptr, TIXML_ENCODING_UTF8);
  auto root = doc.RootElement();

  // Normalize original urdf_string_
  TiXmlPrinter orig_urdf;
  doc.Accept(&orig_urdf);

  // Map existing SimpleTransmission elements to their joint name
  std::map<std::string, TiXmlElement*> transmission_elements;
  for (TiXmlElement* element = root->FirstChildElement("transmission"); element != nullptr;
       element = element->NextSiblingElement(element->Value()))
  {
    auto type_tag = element->FirstChildElement("type");
    auto joint_tag = element->FirstChildElement("joint");
    if (!type_tag || !type_tag->GetText() || !joint_tag || !joint_tag->Attribute("name"))
      continue;  // ignore invalid tags
    if (std::string(type_tag->GetText()) == "transmission_interface/SimpleTransmission")
      transmission_elements[element->FirstChildElement("joint")->Attribute("name")] = element;
  }

  // Loop through Link and Joint elements and add Gazebo tags if not present
  for (TiXmlElement* element = root->FirstChildElement(); element != nullptr; element = element->NextSiblingElement())
  {
    const std::string tag_name(element->Value());
    if (tag_name == "link" && element->FirstChildElement("collision"))
    {
      TiXmlElement* inertial = uniqueInsert(*element, "inertial");
      uniqueInsert(*inertial, "mass", { { "value", "0.1" } });
      uniqueInsert(*inertial, "origin", { { "xyz", "0 0 0" }, { "rpy", "0 0 0" } });
      uniqueInsert(*inertial, "inertia",
                   { { "ixx", "0.03" },
                     { "iyy", "0.03" },
                     { "izz", "0.03" },
                     { "ixy", "0.0" },
                     { "ixz", "0.0" },
                     { "iyz", "0.0" } });
    }
    else if (tag_name == "joint")
    {
      const char* joint_type = element->Attribute("type");
      const char* joint_name = element->Attribute("name");
      if (!joint_type || !joint_name || strcmp(joint_type, "fixed") == 0)
        continue;  // skip invalid or fixed joints

      // find existing or create new transmission element for this joint
      TiXmlElement* transmission;
      auto it = transmission_elements.find(joint_name);
      if (it != transmission_elements.end())
        transmission = it->second;
      else
      {
        transmission = root->InsertEndChild(TiXmlElement("transmission"))->ToElement();
        transmission->SetAttribute("name", std::string("trans_") + joint_name);
      }

      uniqueInsert(*transmission, "type", {}, "transmission_interface/SimpleTransmission");

      std::string hw_interface = getJointHardwareInterface(joint_name);
      auto* joint = uniqueInsert(*transmission, "joint", { { "name", joint_name } });
      uniqueInsert(*joint, "hardwareInterface", {}, hw_interface.c_str());

      auto actuator_name = joint_name + std::string("_motor");
      auto* actuator = uniqueInsert(*transmission, "actuator", { { "name", actuator_name.c_str() } });
      uniqueInsert(*actuator, "hardwareInterface", {}, hw_interface.c_str());
      uniqueInsert(*actuator, "mechanicalReduction", {}, "1");
    }
  }

  // Add gazebo_ros_control plugin which reads the transmission tags
  TiXmlElement* gazebo = uniqueInsert(*root, "gazebo");
  TiXmlElement* plugin = uniqueInsert(*gazebo, "plugin", { { "name", "gazebo_ros_control", true } });
  uniqueInsert(*plugin, "robotNamespace", {}, "/");

  // generate new URDF
  TiXmlPrinter new_urdf;
  doc.Accept(&new_urdf);
  // and return it when there are changes
  return orig_urdf.Str() == new_urdf.Str() ? std::string() : new_urdf.Str();
}

// ******************************************************************************************
// Output Gazebo URDF file
// ******************************************************************************************
bool Simulation::outputGazeboURDFFile(const std::string& file_path)
{
  std::ofstream os(file_path.c_str(), std::ios_base::trunc);
  if (!os.good())
  {
    RCLCPP_ERROR_STREAM(*logger_, "Unable to open file for writing " << file_path);
    return false;
  }

  os << gazebo_urdf_string_.c_str() << std::endl;
  os.close();

  return true;
}

bool Simulation::isValidXML(const std::string& new_urdf_contents, int& error_row, int& error_col,
                            std::string& error_description) const
{
  TiXmlDocument doc;
  doc.Parse(new_urdf_contents.c_str(), nullptr, TIXML_ENCODING_UTF8);
  if (doc.Error())
  {
    error_row = doc.ErrorRow();
    error_col = doc.ErrorCol();
    error_description = doc.ErrorDesc();
  }
  return !doc.Error();
}

}  // namespace moveit_setup_simulation
