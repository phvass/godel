/*
	Copyright Apr 14, 2014 Southwest Research Institute

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at

		http://www.apache.org/licenses/LICENSE-2.0

	Unless required by applicable law or agreed to in writing, software
	distributed under the License is distributed on an "AS IS" BASIS,
	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	See the License for the specific language governing permissions and
	limitations under the License.
*/

#include <godel_surface_detection/scan/robot_scan.h>
#include <pcl_ros/transforms.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/common/common.h>
#include <pcl/filters/filter.h>
#include <boost/assign/list_of.hpp>
#include <boost/assert.hpp>
#include <math.h>


namespace godel_surface_detection {
namespace scan {


const double RobotScan::PLANNING_TIME = 60.0f;
const double RobotScan::WAIT_MSG_DURATION = 2.0f;

RobotScan::RobotScan():
	group_name_("manipulator"),
	world_frame_("world_frame"),
	tcp_frame_("tcp"),
	tcp_to_cam_pose_(tf::Transform::getIdentity()),
	world_to_obj_pose_(tf::Transform::getIdentity()),
	cam_to_obj_zoffset_(0),
	cam_to_obj_xoffset_(0),
	cam_tilt_angle_(-M_PI/4),
	sweep_angle_start_(0),
	sweep_angle_end_(2*M_PI),
	scan_topic_("point_cloud"),
	scan_target_frame_("world_frame"),
	reachable_scan_points_ratio_(0.5f),
	num_scan_points_(20),
	stop_on_planning_error_(true)
		{
	// TODO Auto-generated constructor stub

}

RobotScan::~RobotScan() {
	// TODO Auto-generated destructor stub
}

bool RobotScan::init()
{
	move_group_ptr_ = MoveGroupPtr(new move_group_interface::MoveGroup(group_name_));
	move_group_ptr_->setEndEffectorLink(tcp_frame_);
	move_group_ptr_->setPoseReferenceFrame(world_frame_);
	move_group_ptr_->setPlanningTime(PLANNING_TIME);
	tf_listener_ptr_ = TransformListenerPtr(new tf::TransformListener());
	scan_traj_poses_.clear();
	callback_list_.clear();
	return true;
}

bool RobotScan::load_parameters(std::string ns)
{
	ros::NodeHandle nh(ns);

	// scan transformation
	std::string scan_target_frame_;
	XmlRpc::XmlRpcValue tcp_to_cam_param, world_to_obj_param;
	bool succeeded = nh.getParam("group_name",group_name_) &&
			nh.getParam("world_frame",world_frame_) &&
			nh.getParam("tcp_frame",tcp_frame_) &&
			nh.getParam("tcp_to_cam_pose",tcp_to_cam_param)&&
			nh.getParam("world_to_obj_pose",world_to_obj_param) &&
			nh.getParam("cam_to_obj_zoffset",cam_to_obj_zoffset_) &&
			nh.getParam("cam_to_obj_xoffset",cam_to_obj_xoffset_) &&
			nh.getParam("cam_tilt_angle",cam_tilt_angle_) &&
			nh.getParam("sweep_angle_start",sweep_angle_start_) &&
			nh.getParam("sweep_angle_end",sweep_angle_end_) &&
			nh.getParam("scan_topic",scan_topic_) &&
			nh.getParam("num_scan_points",num_scan_points_) &&
			nh.getParam("reachable_scan_points_ratio",reachable_scan_points_ratio_) &&
			nh.getParam("scan_target_frame",scan_target_frame_) &&
			nh.getParam("stop_on_planning_error",stop_on_planning_error_);

	// parsing poses
	succeeded = succeeded && parse_pose_parameter(tcp_to_cam_param,tcp_to_cam_pose_) &&
			parse_pose_parameter(world_to_obj_param,world_to_obj_pose_);

	return succeeded;
}

void RobotScan::add_scan_callback(ScanCallback cb)
{
	callback_list_.push_back(cb);
}

bool RobotScan::get_scan_trajectory(moveit_msgs::DisplayTrajectory &traj_data)
{
	// create trajectory
	scan_traj_poses_.clear();
	bool succeeded = true;
	moveit_msgs::RobotTrajectory robot_traj;
	if(create_scan_trajectory(scan_traj_poses_,robot_traj))
	{
		traj_data.trajectory.push_back(robot_traj);
	}
	else
	{
		succeeded = false;
	}
	return succeeded;
}

void RobotScan::get_scan_poses(geometry_msgs::PoseArray& poses)
{
	// create trajectory
	scan_traj_poses_.clear();
	bool succeeded = true;
	moveit_msgs::RobotTrajectory robot_traj;
	create_scan_trajectory(scan_traj_poses_,robot_traj);
	for(std::vector<geometry_msgs::Pose>::iterator i = scan_traj_poses_.begin();
			i != scan_traj_poses_.end(); i++)
	{
		poses.poses.push_back(*i);
	}
	poses.header.frame_id = world_frame_;
}

void RobotScan::publish_scan_poses(std::string topic)
{
	ros::NodeHandle nh;
	ros::Publisher poses_pub = nh.advertise<geometry_msgs::PoseArray>(topic,1,true);
	geometry_msgs::PoseArray poses_msg;
	get_scan_poses(poses_msg);
	poses_msg.header.frame_id = world_frame_;
	poses_pub.publish(poses_msg);
	ros::Duration(1.0f).sleep();
}

bool RobotScan::move_to_pose(geometry_msgs::Pose& target_pose)
{
	move_group_ptr_->setPoseTarget(target_pose,tcp_frame_);
	return move_group_ptr_->move();
}

int RobotScan::scan(bool move_only)
{

	// create trajectory
	scan_traj_poses_.clear();
	int poses_reached = 0;
	moveit_msgs::RobotTrajectory robot_traj;
	if(create_scan_trajectory(scan_traj_poses_,robot_traj))
	{
		for(int i = 0;i < scan_traj_poses_.size();i++)
		{
			move_group_ptr_->setPoseTarget(scan_traj_poses_[i],tcp_frame_);

			if(move_group_ptr_->move())
			{
				poses_reached++;
			}
			else
			{
				if(stop_on_planning_error_)
				{
					ROS_ERROR_STREAM("Planning error encountered, quitting scan");
					break;
				}
				else
				{
					ROS_WARN_STREAM("Move to scan position "<<i <<" failed, skipping scan");
					continue;
				}
			}

			if(!move_only)
			{
				// get message
				sensor_msgs::PointCloud2ConstPtr msg = ros::topic::waitForMessage<sensor_msgs::PointCloud2>(scan_topic_,ros::Duration(WAIT_MSG_DURATION));
				pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_ptr(new pcl::PointCloud<pcl::PointXYZ>());
				tf::StampedTransform source_to_target_tf;
				if(msg)
				{
					ROS_INFO_STREAM("Cloud message received, converting to target frame '"<< scan_target_frame_<<"'");

					// convert to message to point cloud
					pcl::fromROSMsg<pcl::PointXYZ>(*msg,*cloud_ptr);

					// removed nans
					std::vector<int> index;
					pcl::removeNaNFromPointCloud(*cloud_ptr,*cloud_ptr,index);

					// transforming
					if(msg->header.frame_id.compare(scan_target_frame_) != 0)
					{
						try
						{
							tf_listener_ptr_->lookupTransform(scan_target_frame_,msg->header.frame_id,ros::Time(0),source_to_target_tf);
							pcl_ros::transformPointCloud(*cloud_ptr,*cloud_ptr,source_to_target_tf);
						}
						catch(tf::LookupException &e)
						{
							ROS_ERROR_STREAM("Transform lookup error, using source frame id '"<< msg->header.frame_id<<"'");
						}
						catch(tf::ExtrapolationException &e)
						{
							ROS_ERROR_STREAM("Transform lookup error, using source frame id '"<< msg->header.frame_id<<"'");
						}
					}

					for(std::vector<ScanCallback>::iterator i = callback_list_.begin(); i != callback_list_.end();i++)
					{
						(*i)(*cloud_ptr);
					}

				}
				else
				{
					ROS_ERROR_STREAM("Cloud message not received");
				}
			}
			else
			{
				ROS_WARN_STREAM("MOVE_ONLY mode, skipping scan");
			}
		}
	}

	return poses_reached;
}

MoveGroupPtr RobotScan::get_move_group()
{
	return move_group_ptr_;
}

bool RobotScan::create_scan_trajectory(std::vector<geometry_msgs::Pose> &scan_poses,moveit_msgs::RobotTrajectory& scan_traj)
{
	// creating poses
	tf::Transform world_to_tcp = tf::Transform::getIdentity();
	tf::Transform world_to_cam = tf::Transform::getIdentity();
	tf::Transform obj_to_cam_pose = tf::Transform::getIdentity();
	geometry_msgs::Pose pose;
	double alpha;
	double alpha_incr = (sweep_angle_end_ - sweep_angle_start_)/(num_scan_points_ -1);
	double eef_step = 4*alpha_incr*cam_to_obj_xoffset_;
	double jump_threshold = 0.0f;


	// relative transforms
	tf::Transform xoffset_disp = tf::Transform(tf::Quaternion::getIdentity(),tf::Vector3(cam_to_obj_xoffset_,0,0));
	tf::Transform zoffset_disp = tf::Transform(tf::Quaternion::getIdentity(),tf::Vector3(0,0,cam_to_obj_zoffset_));
	tf::Transform rot_alpha_about_z = tf::Transform::getIdentity();
	tf::Transform rot_tilt_about_y = tf::Transform(tf::Quaternion(tf::Vector3(0,1,0),cam_tilt_angle_));
	for(int i = 0; i < num_scan_points_;i++)
	{
		alpha = sweep_angle_start_ + alpha_incr * i;
		rot_alpha_about_z = tf::Transform(tf::Quaternion(tf::Vector3(0,0,1),alpha));
		obj_to_cam_pose = zoffset_disp * rot_alpha_about_z*xoffset_disp*rot_tilt_about_y;
		world_to_tcp = world_to_obj_pose_ * obj_to_cam_pose * tcp_to_cam_pose_.inverse();
		tf::poseTFToMsg(world_to_tcp,pose);
		scan_poses.push_back(pose);
	}

	ROS_INFO_STREAM("Computing cartesian path for a trajectory with "<<num_scan_points_<<" points");
	double res = move_group_ptr_->computeCartesianPath(scan_poses,eef_step,jump_threshold,scan_traj,true);
	double success = res >= reachable_scan_points_ratio_;
	if(success)
	{
		ROS_INFO_STREAM("Reachable scan poses percentage "<<res<<" is at or above the acceptance threshold of "<<reachable_scan_points_ratio_);
	}
	else
	{
		ROS_WARN_STREAM("Reachable scan poses percentage "<<res<<" is below the acceptance threshold of "<<reachable_scan_points_ratio_);
	}

	return success;
}

bool RobotScan::parse_pose_parameter(XmlRpc::XmlRpcValue pose_param,tf::Transform &t)
{
	std::map<std::string,double> fields_map =
			boost::assign::map_list_of("x",0.0d)
			("y",0.0d)
			("z",0.0d)
			("rx",0.0d)
			("ry",0.0d)
			("rz",0.0d);

	// parsing fields
	std::map<std::string,double>::iterator i;
	bool succeeded = true;
	for(i= fields_map.begin();i != fields_map.end();i++)
	{
		if(pose_param.hasMember(i->first) && pose_param[i->first].getType() == XmlRpc::XmlRpcValue::TypeDouble)
		{
			fields_map[i->first] = static_cast<double>(pose_param[i->first]);
		}
		else
		{
			succeeded = false;
			break;
		}
	}

	tf::Vector3 pos = tf::Vector3(fields_map["x"],fields_map["y"],fields_map["z"]);
	tf::Quaternion q;
	q.setRPY(fields_map["rx"],fields_map["ry"],fields_map["rz"]); // fixed axis
	t.setOrigin(pos);
	t.setRotation(q);

	return succeeded;
}

} /* namespace scan */
} /* namespace godel_surface_detection */
