/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2015,
 *  TU Dortmund - Institute of Control Theory and Systems Engineering.
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
 *   * Neither the name of the institute nor the names of its
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
 *
 * Author: Christoph Rösmann
 *********************************************************************/

#include <phantomx_rst/phantomx_interface.h>


namespace phantomx
{
  
PhantomXControl::PhantomXControl()
{
}

PhantomXControl::~PhantomXControl()
{
  if (_joints_sub_spinner)
    _joints_sub_spinner->stop();
  if (_arm_action)
    _arm_action->cancelAllGoals();
}
  
void PhantomXControl::initialize()
{
  // instantiate arm action client
  ROS_INFO("Waiting for arm action server to start.");
  _arm_action = make_unique<actionlib::SimpleActionClient<control_msgs::FollowJointTrajectoryAction>>("/arm_controller/follow_joint_trajectory", true);
  _arm_action->waitForServer(); // will wait for infinite time

  
  // instantiate gripper action client
  ROS_INFO("Arm action server found. Waiting for gripper action server to start.");
  _gripper_action = make_unique<actionlib::SimpleActionClient<control_msgs::GripperCommandAction>>("/gripper_controller/gripper_action", true);
  _gripper_action->waitForServer(); // will wait for infinite time
  ROS_INFO("All action servers started.");
  
  // setup subscriber (get joint_states in a separate thread)
  ros::NodeHandle n;
  _joints_sub_queue = new ros::CallbackQueue();
  n.setCallbackQueue(_joints_sub_queue);
  _joints_sub = n.subscribe("/joint_states", 1, &PhantomXControl::jointStateCallback, this);
  _joints_sub_spinner = make_unique<ros::AsyncSpinner>(1, _joints_sub_queue);
  ROS_ERROR_COND(!_joints_sub_spinner->canStart(),"Asynchronous spinner for reveiving new joint state messages cannot be started.");
  _joints_sub_spinner->start();

  // setup joint names
  _joint_names_arm = {"arm_shoulder_pan_joint",
		      "arm_shoulder_lift_joint",
		      "arm_elbow_flex_joint",
		      "arm_wrist_flex_joint"};
  for (int i=0; i< _joint_names_arm.size(); ++i)
  {
    _map_joint_to_index[_joint_names_arm.at(i)] = i;
  }
   
  // get joint information (angle limits and max speed)
  for (int i=0; i<_joint_names_arm.size(); ++i)
  {
	std::string param_prefix = "/arbotix/joints/" + _joint_names_arm[i]; // TODO config param
        std::string min_angle_key = param_prefix + "/min_angle";
	std::string max_angle_key = param_prefix + "/max_angle";
	std::string max_speed_key = param_prefix + "/max_speed";
	
	if (!n.hasParam(min_angle_key) || !n.hasParam(max_angle_key) || !n.hasParam(max_speed_key))
	  ROS_ERROR("Could not find one or all of the following parameters:\n %s\n %s\n %s", min_angle_key.c_str(), max_angle_key.c_str(), max_speed_key.c_str());
	
	n.getParam(min_angle_key, _joint_lower_bounds[i]);
	n.getParam(max_angle_key, _joint_upper_bounds[i]);
	n.getParam(max_speed_key, _joint_max_speeds[i]);
	
	// convert angles to radiant
	_joint_lower_bounds[i] = normalize_angle_rad( deg_to_rad( _joint_lower_bounds[i] ) ); // normalize to (-pi, pi]
	_joint_upper_bounds[i] = normalize_angle_rad( deg_to_rad( _joint_upper_bounds[i] ) ); // normalize to (-pi, pi]
	_joint_max_speeds[i] = deg_to_rad( _joint_max_speeds[i] );
  }     
  
  // Setup kinematic model
	
  // Wait for joint_state messages
  while (!_joint_values_received && ros::ok())
  {
      ROS_INFO_ONCE("Waiting for joint_states message...");
      _joints_sub_queue->callAvailable();
  }
  // Drive into default position
  ROS_INFO("Driving into default position in order to setup kinematic model...");
  setJointsDefault(0.2);

  
  // get transform: base to first joint 
  tf::StampedTransform transform;
  try
  {
    _tf.waitForTransform("/arm_base_link", "/arm_shoulder_pan_servo_link", ros::Time(0), ros::Duration(10.0) );
    _tf.lookupTransform("/arm_base_link", "/arm_shoulder_pan_servo_link", ros::Time(0), transform); // TODO: param
  }
  catch (tf::TransformException &ex)
  {
    ROS_ERROR("%s",ex.what());
  }
  Eigen::Affine3d base_T_arm;
  tf::transformTFToEigen(transform, base_T_arm);
  _kinematics.setBaseToJoint1Transform(base_T_arm);

  // get transform: first joint to second joint
  try
  {
    _tf.waitForTransform("/arm_shoulder_pan_servo_link", "/arm_shoulder_lift_servo_link", ros::Time(0), ros::Duration(10.0) );
    _tf.lookupTransform("/arm_shoulder_pan_servo_link", "/arm_shoulder_lift_servo_link", ros::Time(0), transform); // TODO: param
  }
  catch (tf::TransformException &ex)
  {
    ROS_ERROR("%s",ex.what());
  }
  Eigen::Affine3d j1_T_j2;
  tf::transformTFToEigen(transform, j1_T_j2);
  _kinematics.setJoint1ToJoint2Transform(j1_T_j2);

  // get transform: second joint to third joint
  try
  {
    _tf.waitForTransform("/arm_shoulder_lift_servo_link", "/arm_elbow_flex_servo_link", ros::Time(0), ros::Duration(10.0) );
    _tf.lookupTransform("/arm_shoulder_lift_servo_link", "/arm_elbow_flex_servo_link", ros::Time(0), transform); // TODO: param
  }
  catch (tf::TransformException &ex)
  {
    ROS_ERROR("%s",ex.what());
  }
  Eigen::Affine3d j2_T_j3;
  tf::transformTFToEigen(transform, j2_T_j3);
  _kinematics.setJoint2ToJoint3Transform(j2_T_j3);

  // get transform: forth joint to second joint
  try
  {
    _tf.waitForTransform("/arm_elbow_flex_servo_link", "/arm_wrist_flex_servo_link", ros::Time(0), ros::Duration(10.0) );
    _tf.lookupTransform("/arm_elbow_flex_servo_link", "/arm_wrist_flex_servo_link", ros::Time(0), transform); // TODO: param
  }
  catch (tf::TransformException &ex)
  {
    ROS_ERROR("%s",ex.what());
  }
  Eigen::Affine3d j3_T_j4;
  tf::transformTFToEigen(transform, j3_T_j4);
  _kinematics.setJoint3ToJoint4Transform(j3_T_j4);

  // get transform: last joint to gripper
  try
  {
    _tf.waitForTransform("/arm_wrist_flex_servo_link", "/gripper_link", ros::Time(0), ros::Duration(10.0) );
    _tf.lookupTransform("/arm_wrist_flex_servo_link", "/gripper_link", ros::Time(0), transform); // TODO: param
  }
  catch (tf::TransformException &ex)
  {
    ROS_ERROR("%s",ex.what());
  }
  Eigen::Affine3d arm_T_gripper;
  tf::transformTFToEigen(transform, arm_T_gripper);
  _kinematics.setJoint4ToGripperTransform(arm_T_gripper);
  
  // notify the kinematics model about lower and upper joint limits
  _kinematics.setLowerAndUpperJointLimits(_joint_lower_bounds, _joint_upper_bounds);
  
  _initialized = true;
  ROS_INFO("Initialization completed.");
}

void PhantomXControl::jointStateCallback(const sensor_msgs::JointStateConstPtr& msg)
{
  std::lock_guard<std::mutex> lock(_joints_mutex);
  for (int i=0; i<msg->name.size(); ++i)
  {
    int joint_idx = _map_joint_to_index[msg->name[i]];
    _joint_angles[joint_idx] = msg->position[i];
  }
  if (!_joint_values_received)
      _joint_values_received = true;
}

void PhantomXControl::getJointAngles(Eigen::Ref<JointVector> values_out)
{
  std::lock_guard<std::mutex> lock(_joints_mutex);
  values_out = _joint_angles;
}

void PhantomXControl::getJointAngles(std::vector<double>& values_out)
{
  std::lock_guard<std::mutex> lock(_joints_mutex);
  values_out.assign(_joint_angles.data(), _joint_angles.data()+_joint_angles.rows());
}

void PhantomXControl::getJointVelocities(Eigen::Ref<JointVector> velocities_out)
{
  std::lock_guard<std::mutex> lock(_joints_mutex);
  velocities_out = _joint_velocities;
}

double PhantomXControl::getSlowestMaxSpeed() const
{
  return _joint_max_speeds.minCoeff();
}

  
void PhantomXControl::setJointsDefault(const ros::Duration& duration, bool blocking)
{
  setJoints(JointVector::Zero(), duration, false, blocking);
}
  
void PhantomXControl::setJointsDefault(double speed, bool blocking)
{
  setJoints(JointVector::Zero(), speed, false, blocking);
}  
  
void PhantomXControl::setJoints(const Eigen::Ref< const JointVector>& values, const ros::Duration& duration, bool relative, bool blocking)
{
  trajectory_msgs::JointTrajectoryPoint state;
  state.positions.assign(values.data(), values.data()+values.rows());
  state.time_from_start = duration;
  setJoints(state, relative, blocking);  
}

void PhantomXControl::setJoints(const std::vector<double>& values, const ros::Duration& duration, bool relative, bool blocking)
{
  Eigen::Map<const JointVector> values_map(values.data());
  setJoints(values_map, duration, relative, blocking);
}

void PhantomXControl::setJoints(const Eigen::Ref<const JointVector>& values, double speed, bool relative, bool blocking)
{
  ROS_ASSERT_MSG(speed>0, "You cannot command a zero velocity.");
//   ROS_ASSERT_MSG(_initialized, "Setting new joints with a desired velocity requires querying current joint values, that is only possible after initialization is completed.");
  
  JointVector current_states;  
  getJointAngles(current_states); // we need to do a copy here, rather accessing '_joint_angles' directly,
				  // since receiving new states is multi threaded.
  
  // get maximum absolute angle difference (TODO: do we need to normalize the angles before taking abs()?)
  double max_diff = (values - current_states).cwiseAbs().maxCoeff();

  if (max_diff<0.001)
      return; // we are already there...

  // get time corresponding to the distace max_diff and the given speed value
  // assume a constant velocity: phi=omega*t
  double duration = max_diff/speed;
  
  if (duration<0)
  {
    ROS_ERROR("PhantomXControl::setJoints(): obtained an invalid (negative) duration. Cannot set new joint values.");
    return;
  }
  setJoints(values,ros::Duration(duration), relative, blocking);
}
  
void PhantomXControl::setJoints(const std::vector<double>& values, double speed, bool relative, bool blocking)
{
  Eigen::Map<const JointVector> values_map(values.data());
  setJoints(values_map, speed, relative, blocking);
}  
  
  
void PhantomXControl::setJoints(const Eigen::Ref<const JointVector>& values, const Eigen::Ref<const JointVector>& speed, bool relative, bool blocking)
{
  trajectory_msgs::JointTrajectoryPoint state;
  state.positions.assign(values.data(), values.data()+values.rows());
  state.velocities.assign(speed.data(), speed.data()+speed.rows());
  state.time_from_start = ros::Duration(0); // use individual velocities
  setJoints(state, relative, blocking);
}
  
void PhantomXControl::setJoints(const std::vector<double>& values, const std::vector<double>& speed, bool relative, bool blocking)
{
  Eigen::Map<const JointVector> values_map(values.data());
  Eigen::Map<const JointVector> vel_map(speed.data());
  setJoints(values_map, vel_map, relative, blocking);
}
  

void PhantomXControl::setJoints(const trajectory_msgs::JointTrajectoryPoint& joint_state, bool relative, bool blocking)
{
  bool sync_duration_mode = joint_state.time_from_start.sec!=0 || joint_state.time_from_start.nsec!=0;
  if ( (sync_duration_mode && !joint_state.velocities.empty()) || (!sync_duration_mode && joint_state.velocities.empty()) )
  {
    ROS_ERROR("PhantomXControl::setJoints(): you must either specify a total duration (time_from_start) or individual velocities. Do not choose both.");
    return;
  }
  
  // create trajectory to the single goal
  control_msgs::FollowJointTrajectoryGoal goal;
  goal.trajectory.joint_names = _joint_names_arm;
  goal.trajectory.header.stamp = ros::Time::now();
  goal.trajectory.points.push_back(joint_state);
  
  trajectory_msgs::JointTrajectoryPoint& new_state = goal.trajectory.points.front(); // get reference for further calculations
                                                                                     // since joint_state argument is read-only, we modify it here.
  
  // Get current joint angles 
  JointVector current_states;  
  getJointAngles(current_states); // we need to do a copy here, rather accessing '_joint_angles' directly,
                                  // since receiving new states is multi threaded.
  
  Eigen::Map<JointVector> values(new_state.positions.data()); // get an Eigen map for the position part for further computations.
  
  if (relative)
      values -= current_states; // this also changes values in "goal"
  
  // check joint angle limits
  if (isExceedingJointLimits(values))
  {
    ROS_WARN_STREAM("PhantomXControl::setJoints(): cannot set new joint values since they exceed joint limits.\ndesired angles: ["
		    << values.transpose() << "]\nlower bounds: [" 
		    << _joint_lower_bounds.transpose() << "]\nupper bounds: [" << _joint_upper_bounds.transpose() << "]");
    return;
  }  
     
  // check velocities and/or duration before adding
  for (int i=0; i<new_state.velocities.size();++i)
  {
    new_state.velocities[i] = fabs(new_state.velocities[i]); // we consider only absolute velocities
    if (new_state.velocities[i] > _joint_max_speeds[i])
    {
      ROS_WARN("PhantomXControl::setJoints(): Velocity of joint %d exceeds limits. Bounding...", i);
      new_state.velocities[i] = _joint_max_speeds.coeffRef(i);
    }
  }
  if (sync_duration_mode)
  {
      // get maximum absolute angle difference
      
      double max_diff = (values - current_states).cwiseAbs().maxCoeff();

      // check and bound velocity
      // get maximum allowed duration (w.r.t. min of all max joint speeds)
      double duration_bounded = lower_bound(max_diff / getSlowestMaxSpeed(), new_state.time_from_start.toSec()); // phi = omega*t -> t = phi/omega
      ROS_WARN_COND(duration_bounded>new_state.time_from_start.toSec(), "PhantomXControl::setJoints(): desired speed is too high in order to drive all joints at this speed. Bounding...");
      new_state.time_from_start = ros::Duration(duration_bounded);
  }
  
  setJointTrajectory(goal, blocking);
}
 
  
void PhantomXControl::setJointVel(const Eigen::Ref<const JointVector>& velocities)
{
  // Select lower and upper bound values for each joint as goal w.r.t. the direction of the velocity
  // For zero velocities stay at the current position
  JointVector current;
  getJointAngles(current);
  JointVector goal = (velocities.array()==0).select( current, (velocities.array()<0).select(_joint_lower_bounds,_joint_upper_bounds) );
  setJoints(goal, velocities, false, false); // we do not want to block in case of commanding velocities
}
  
void PhantomXControl::setJointVel(const std::vector<double>& velocities)
{
  Eigen::Map<const JointVector> vel_map(velocities.data());
  setJointVel(vel_map);
}  
  
  
void PhantomXControl::setJointTrajectory(trajectory_msgs::JointTrajectory& trajectory, bool blocking)
{
  control_msgs::FollowJointTrajectoryGoal goal;
  goal.trajectory = trajectory;
  
  setJointTrajectory(goal, blocking);
}

void PhantomXControl::setJointTrajectory(control_msgs::FollowJointTrajectoryGoal& trajectory, bool blocking)
{
  ROS_ASSERT_MSG(_arm_action, "PhantomXControl: class not initialized, call initialize().");
    
  // cancel any previous goals
  stopMoving();
  
  // verify trajectory and adjust speeds if necessary
  bool feasible = verifyTrajectory(trajectory.trajectory); // returns false if angle limits are exceeded or something goes wrong
  if (!feasible)
    return; // Errors are printed in verifyTrajectory()
  
  if (blocking)
    _arm_action->sendGoalAndWait(trajectory, ros::Duration(20));
  else
    _arm_action->sendGoal(trajectory);
}


bool PhantomXControl::isExceedingJointLimits(const Eigen::Ref<const JointVector>& joint_values)
{
  return (joint_values.array()<_joint_lower_bounds.array()).any() || (joint_values.array()>_joint_upper_bounds.array()).any();
}

bool PhantomXControl::verifyTrajectory(trajectory_msgs::JointTrajectory& trajectory)
{
  // Get current joint angles 
  JointVector current_states;
  getJointAngles(current_states); // we need to do a copy here, rather accessing '_joint_angles' directly,
				  // since receiving new states is multi threaded.
  
  // Store previous angle positions as Eigen type (required inside the loop)
  Eigen::Map<JointVector> prev_pos(current_states.data());  
 
  for (trajectory_msgs::JointTrajectoryPoint& state : trajectory.points)
  {
      // check transition mode (synchronous transition or individual velocities
      bool sync_duration_mode = state.time_from_start.sec!=0 || state.time_from_start.nsec!=0;
      if ( (sync_duration_mode && !state.velocities.empty()) || (!sync_duration_mode && state.velocities.empty()) )
      {
	ROS_ERROR("PhantomXControl::setJointTrajectory(): you must either specify a total duration (time_from_start) or individual velocities. Do not choose both.");
	return false;
      }
      
      Eigen::Map<const JointVector> values(state.positions.data()); // get an Eigen map for the position part for further computations.
      
      // check joint angle limits
      if (isExceedingJointLimits(values))
      {
	ROS_WARN_STREAM("PhantomXControl::setJointTrajectory(): cannot set new joint values since they exceed joint limits.\ndesired angles: ["
			<< values.transpose() << "]\nlower bounds: [" 
			<< _joint_lower_bounds.transpose() << "]\nupper bounds: [" << _joint_upper_bounds.transpose() << "]");
	return false;
      }  
      
      // check velocities and/or duration before adding
      for (int i=0; i<state.velocities.size();++i)
      {
	state.velocities[i] = fabs(state.velocities[i]); // we consider only absolute velocities
	if (state.velocities[i] > _joint_max_speeds[i])
	{
	  ROS_WARN("PhantomXControl::setJoints(): Velocity of joint %d exceeds limits. Bounding...", i);
	  state.velocities[i] = _joint_max_speeds.coeffRef(i);
	}
      }
      if (sync_duration_mode)
      {
	  // get maximum absolute angle difference 
	  double max_diff = (values - prev_pos).cwiseAbs().maxCoeff();

	  // check and bound velocity
	  // get maximum allowed duration (w.r.t. min of all max joint speeds)
	  double duration_bounded = lower_bound(max_diff / getSlowestMaxSpeed(), state.time_from_start.toSec()); // phi = omega*t -> t = phi/omega
	  ROS_WARN_COND(duration_bounded>state.time_from_start.toSec(), "PhantomXControl::setJoints(): desired speed is too high in order to drive all joints at this speed. Bounding...");
	  state.time_from_start = ros::Duration(duration_bounded);
      }
      
      // Store pos vector for the subsequent iteration (using C++ placement new operator)
      new (&prev_pos) Eigen::Map<JointVector>(state.positions.data());
  }
  return true;
}
  
  
void PhantomXControl::stopMoving()
{
    _arm_action->cancelAllGoals();
    ros::Duration(0.001).sleep(); // we wait for a small time, since cancelling goal is scheduled in a separate thread.
				  // But this small duration does not guarantee, that a goal immediately set after calling stop stopMoving
				  // is actually excecuted.
}
  
  
void PhantomXControl::getEndeffectorState(Eigen::Affine3d& base_T_gripper)
{
    // get tf::Transform
    tf::StampedTransform tf_transform;
    getEndeffectorState(tf_transform);
    tf::transformTFToEigen(tf_transform, base_T_gripper);
}
  
void PhantomXControl::getEndeffectorState(tf::StampedTransform& base_T_gripper)
{
    try
    {
      _tf.waitForTransform("/arm_base_link", "/gripper_link", ros::Time(0), ros::Duration(10.0) );
      _tf.lookupTransform("/arm_base_link", "/gripper_link", ros::Time(0), base_T_gripper); // TODO: param
    }
    catch (tf::TransformException &ex)
    {
      ROS_ERROR("%s",ex.what());
    }
}


void PhantomXControl::getJacobian(RobotJacobian& jacobian)
{
    JointVector joint_angles;
    getJointAngles(joint_angles);
    kinematics().computeJacobian(joint_angles, jacobian);
}

bool PhantomXControl::testKinematicModel()
{

  bool retval = true;
  
  // Default position
  JointVector values;
  values.setZero();
  retval = retval && testKinematicModel(values);
     
  // Pos 1
  values << -M_PI/2, 0, 0, 0;
  retval = retval && testKinematicModel(values);
  
  // Pos 2
  values << 0, M_PI/3, 0, 0;
  retval = retval && testKinematicModel(values);
  
  // Pos 3
  values << 0, 0, M_PI/2, 0;
  retval = retval && testKinematicModel(values);
  
  // Pos 4
  values << 0, 0, 0, -M_PI/2;
  retval = retval && testKinematicModel(values);
  
  // Pos 5
  values << M_PI/2, -M_PI/3, M_PI/5, -M_PI/2;
  retval = retval && testKinematicModel(values);
  
  if (retval)
    ROS_INFO_STREAM("testKinematicModel successfully completed.");
  else
    ROS_INFO_STREAM("testKinematicModel completed with errors.");
  
  return retval;
}
  
bool PhantomXControl::testKinematicModel(const Eigen::Ref<const JointVector>& joint_values)
{
  bool retval = true;

  ROS_INFO_STREAM("Testing joint configuration: q=[" << joint_values.transpose() << "]...");
  
  // command robot
  setJoints(joint_values,ros::Duration(5));
  // get current state
  Eigen::Affine3d real;
  getEndeffectorState(real);
  // compute forward kinematics using the model class
  Eigen::Affine3d model = kinematics().computeForwardKinematics(joint_values);
  bool trans_flag = model.translation().isApprox( real.translation(), 0.1 );
  if ( !trans_flag)
  {
    ROS_WARN_STREAM("Translation part does not match\nreal: [" << real.translation().transpose()
                    << "] model: [" << model.translation().transpose() << "]");
    retval = false;
  }
  Eigen::Quaterniond qreal = Eigen::Quaterniond(real.rotation());
  Eigen::Quaterniond qmodel = Eigen::Quaterniond(model.rotation());
  bool rot_flag = qmodel.isApprox( qreal, 0.1 ) || qmodel.isApprox( Eigen::Quaterniond(-qreal.w(),-qreal.x(),-qreal.y(),-qreal.z()), 0.1 );
  if ( !rot_flag )
  {
    ROS_WARN_STREAM("Rotation part does not match\nreal: [" << qreal.w() << "," << qreal.vec().transpose()
             << "] model: [" << qmodel.w() << "," << qmodel.vec().transpose() << "]");
    tf::Quaternion tf_qmodel;
    tf::quaternionEigenToTF(qmodel,tf_qmodel);
    ROS_WARN_STREAM("yaw: " << tf::getYaw(tf_qmodel));
    retval = false;
  }
  ROS_INFO("Test finished.");
  return retval;  
}
  
  
} // end namespace phantomx