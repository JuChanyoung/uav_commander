/*********************************************************************************************
* Copyright (C) 2013 - 2014 by                                                               *
* Bara Emran, Rui P. de Figueiredo and Tarek Taha Khalifa University Robotics Institute KURI *
* <bara.emran@kustar.ac.ae>, <rui.defigueiredo@kustar.ac.ae> <tarek.taha@kustar.ac.ae>       *                     	  *
*                                                                          	                 *
*                                                                           	             *
* This program is free software; you can redistribute it and/or modify     	                 *
* it under the terms of the GNU General Public License as published by     	                 *
* the Free Software Foundation; either version 2 of the License, or        	                 *
* (at your option) any later version.                                      	                 *
*                                                                          	                 *
* This program is distributed in the hope that it will be useful,          	                 *
* but WITHOUT ANY WARRANTY; without even the implied warranty of           	                 *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the             	                 *
* GNU General Public License for more details.                              	             *
*                                                                          	                 *
* You should have received a copy of the GNU General Public License        	                 *
* along with this program; if not, write to the                            	                 *
* Free Software Foundation, Inc.,                                          	                 *
* 51 Franklin Steet, Fifth Floor, Boston, MA 02111-1307, USA.              	                 *
**********************************************************************************************/
#include "ros/ros.h"
#include "std_msgs/String.h"
#include "std_msgs/Empty.h"

#include "std_msgs/Float64MultiArray.h"
#include "visualeyez_tracker/TrackerPose.h"
#include "geometry_msgs/PoseStamped.h"
#include "geometry_msgs/Pose.h"
#include "geometry_msgs/Twist.h"
#include "uav_commander/ControlInfo.h"
#include "nav_msgs/Odometry.h"
#include <Eigen/Eigen>
#include <std_msgs/String.h>
#include <dynamic_reconfigure/server.h>
#include <uav_commander/PIDControlConfig.h>
#include <actionlib/server/simple_action_server.h>
#include <uav_commander/WayPointAction.h>

//#include "pctx_control/Control.h"

#include <sstream>
#include <sys/time.h>

#define PI 3.14159265
#define BILLION 1000000000
double EPS = 0.2;

double Sat (double num, double Max , double Min);
bool error(const Eigen::Matrix<double,6,1> Pose, const Eigen::Matrix<double,6,1> Target);

class PositionCommand
{
public: 

    int     count;
    bool    init_;
    bool    control_,control_gui_enable_;
    bool    got_pose_update_;
    bool    got_goal_;
    double  period_;
    bool    load_gains_;
    bool    action_recived_;
    Eigen::Matrix<double,6,1> Kp;
    Eigen::Matrix<double,6,1> Kd;
    Eigen::Matrix<double,6,1> Ki;
    Eigen::Matrix<double,6,1> goal_pose_;
    Eigen::Matrix<double,6,1> current_pose_;
    Eigen::Matrix<double,6,1> previous_pose_;
    Eigen::Matrix<double,6,1> accum_error_;
    Eigen::Matrix<double,6,1> previous_error_;
    Eigen::Matrix<double,3,3> rot_matrix_;

    ros::NodeHandle n_;
    ros::Subscriber goal_sub;
    ros::Subscriber pose_sub;
    ros::Publisher  vel_cmd;
    ros::Publisher  control_info_pub;
    ros::Publisher  current_error_pub ;
    ros::Publisher  takeoff_pub,land_pub,reset_pub;
    ros::Publisher  des_pose_pub;
    ros::Time       previous_time_;
    geometry_msgs::PoseStamped C_Pose_;
    uav_commander::ControlInfo control_info_msg;

    dynamic_reconfigure::Server<uav_commander::PIDControlConfig> server;
    dynamic_reconfigure::Server<uav_commander::PIDControlConfig>::CallbackType dynamic_function;

    actionlib::SimpleActionServer <uav_commander::WayPointAction> as_;
    uav_commander::WayPointFeedback feedback_;
    uav_commander::WayPointResult   result_;
    std::string action_name_;

    PositionCommand(ros::NodeHandle & n,std::string name);
    void control();

private:
    void getGoal   (const  geometry_msgs::PoseStamped::ConstPtr    & goal);
    void getPose   (const  geometry_msgs::PoseStamped::ConstPtr    & Pose);
    void dynamic   (       uav_commander::PIDControlConfig         & config, uint32_t level);

    void goalCB    ();

};

PositionCommand::PositionCommand(ros::NodeHandle & n,std::string name) :
    n_(n), control_(false), init_(true),
    action_name_(name),
    as_(n, name, false)
{
    ROS_INFO("initialization");

    pose_sub            = n_.subscribe("pose"   , 1, &PositionCommand::getPose   , this);
    goal_sub            = n_.subscribe("goal"   , 1, &PositionCommand::getGoal   , this);
    vel_cmd             = n_.advertise <geometry_msgs::Twist       > ("/cmd_vel"        , 1);
    control_info_pub    = n_.advertise <uav_commander::ControlInfo > ("/control_info"   , 1);
    current_error_pub   = n_.advertise <geometry_msgs::Pose        > ("/current_error"  , 1);
    takeoff_pub         = n_.advertise <std_msgs::Empty            > ("/ardrone/takeoff", 1);
    land_pub            = n_.advertise <std_msgs::Empty            > ("/ardrone/land"   , 1);
    reset_pub           = n_.advertise <std_msgs::Empty            > ("/ardrone/reset"  , 1);
    des_pose_pub        = n_.advertise <geometry_msgs::Pose > ("/uav/des_pose"   , 1);

    //register the goal and feeback callbacks
    as_.registerGoalCallback   (boost::bind(&PositionCommand::goalCB, this));
    as_.start();

    ROS_INFO("START");
    Kp << 0.5  , 0.5  , 0.5  , 0 , 0 , 0 ;
    Ki << 0.02 , 0.02 , 0.02 , 0 , 0 , 0;
    Kd << 0.3  , 0.3  , 0.3  , 0 , 0 , 0;

    previous_error_  << 0.0, 0.0, 0.0, 0.0, 0.0, 0.0;
    accum_error_     << 0.0, 0.0, 0.0, 0.0, 0.0, 0.0;

    dynamic_function = boost::bind(&PositionCommand::dynamic,this, _1, _2);
    server.setCallback(dynamic_function);
    return;
}


void PositionCommand::control()
{
    geometry_msgs::Twist        vel_cmd_msg;
    geometry_msgs::Pose         current_error_msg;
    Eigen::Matrix<double,6,1>   current_error;
    geometry_msgs::Pose  des_pose_msg;

    if (action_recived_)
    {
        feedback_.pose = C_Pose_;
        as_.publishFeedback(feedback_);

        if(error(current_pose_,goal_pose_))
        {
            result_.pose = feedback_.pose;
            ROS_INFO("%s: Succeeded", action_name_.c_str());
            as_.setSucceeded(result_);
            action_recived_ = false;
        }
    }

    current_error = goal_pose_ - current_pose_;
    if (control_ && control_gui_enable_ )
    {
        accum_error_ = accum_error_+ period_*(current_error + previous_error_)/2.0;

        Eigen::Matrix<double,6,6> Cp = Kp * current_error.transpose();
        Eigen::Matrix<double,6,6> Ci = Ki * accum_error_.transpose();
        Eigen::Matrix<double,6,6> Cd = Kd * (current_error - previous_error_).transpose()/period_;

        Eigen::Matrix<double,6,6> vel_cmd_current = Cp + Ci + Cd ;
        Eigen::Matrix<double,3,1> linear_vel_cmd_l_frame;

        linear_vel_cmd_l_frame << vel_cmd_current(0,0),
                vel_cmd_current(1,1),
                vel_cmd_current(2,2);

        Eigen::Matrix<double,3,1> linear_vel_cmd_g_frame = rot_matrix_.inverse() * linear_vel_cmd_l_frame;

        vel_cmd_msg.linear.x  = Sat(linear_vel_cmd_g_frame(0,0),1,-1);
        vel_cmd_msg.linear.y  = Sat(linear_vel_cmd_g_frame(1,0),1,-1);
        vel_cmd_msg.linear.z  = Sat(linear_vel_cmd_g_frame(2,0),1,-1);
        vel_cmd_msg.angular.z = Sat(vel_cmd_current(5,5),1,-1);

        previous_error_     = current_error;
        control_            = false;
    }

    current_error_msg.position.x    = current_error(0,0);
    current_error_msg.position.y    = current_error(1,0);
    current_error_msg.position.z    = current_error(2,0);
    current_error_msg.orientation.x = current_error(0,0);
    current_error_msg.orientation.y = current_error(1,0);
    current_error_msg.orientation.z = current_error(2,0); 
    current_error_pub.publish(current_error_msg);

    des_pose_msg.position.x = goal_pose_(0,0);
    des_pose_msg.position.y = goal_pose_(1,0);
    des_pose_msg.position.z = goal_pose_(2,0);
    des_pose_pub.publish(des_pose_msg);

    vel_cmd.publish(vel_cmd_msg);

    std::cout << "CE: x ="  <<  current_error(0,0)
              << " y = "    <<  current_error(1,0)
              << " z = "    <<  current_error(2,0)
              << " w = "    <<  current_error(5,0)
              << std::endl;

    std::cout << "vel: x =" <<  vel_cmd_msg.linear.x
              << " y = "    <<  vel_cmd_msg.linear.y
              << " z = "    <<  vel_cmd_msg.linear.z
              << " w = "    <<  vel_cmd_msg.angular.z
              << std::endl;

}

void PositionCommand::getPose(const geometry_msgs::PoseStamped::ConstPtr & Pose)
{
    rot_matrix_= Eigen::Quaterniond(Pose->pose.orientation.w,
                                    Pose->pose.orientation.x,
                                    Pose->pose.orientation.y,
                                    Pose->pose.orientation.z).matrix();

    Eigen::Matrix<double,3,1> euler = rot_matrix_.eulerAngles(2, 1, 0);

    double yaw   = euler(0,0);
    double pitch = euler(1,0);
    double roll  = euler(2,0);

    current_pose_ <<    Pose->pose.position.x,
            Pose->pose.position.y,
            Pose->pose.position.z,
            roll,
            pitch,
            yaw;
    control_ = true;

    // if it is the first time: initialize the function
    if(init_)
    {
        ROS_INFO("initialize");
        previous_time_  = ros::Time::now();
        goal_pose_      << 0.0, 0.0, 1.0, 0.0, 0.0, 0.0;
        init_           = false;
        control_        = false;
    }

    ros::Time current_time  = Pose->header.stamp;
    period_                 = current_time.toSec() - previous_time_.toSec();
    previous_time_          = current_time;

    control_info_msg.Period         = period_;
    control_info_msg.header.stamp   = current_time;
    control_info_pub.publish(control_info_msg);
}

void PositionCommand::getGoal(const geometry_msgs::PoseStamped::ConstPtr & goal)
{

    Eigen::Matrix<double,3,1> euler = Eigen::Quaterniond(goal->pose.orientation.w,
                                                         goal->pose.orientation.x,
                                                         goal->pose.orientation.y,
                                                         goal->pose.orientation.z).matrix().eulerAngles(2, 1, 0);
    double yaw   = euler(0,0);
    double pitch = euler(1,0);
    double roll  = euler(2,0);

    goal_pose_ <<   goal->pose.position.x,
            goal->pose.position.y,
            goal->pose.position.z,
            roll,
            pitch,
            yaw;

    ROS_INFO("GOT NEW GOAL");
    std::cout << "goal_pose: " << goal_pose_.transpose()<< std::endl;

}


void PositionCommand::dynamic(uav_commander::PIDControlConfig &config, uint32_t level)
{
    if (config.Load_PID)
    {
        ros::NodeHandle n_priv("~");
        n_priv.param<double>("kp_x" , config.Kp_x , 0.71);
        n_priv.param<double>("kp_y" , config.Kp_y , 0.72);
        n_priv.param<double>("kp_z" , config.Kp_z , 0.73);
        n_priv.param<double>("kp_w" , config.Kp_w , 0.74);
        n_priv.param<double>("ki_x" , config.Ki_x , 0.71);
        n_priv.param<double>("ki_y" , config.Ki_y , 0.72);
        n_priv.param<double>("ki_z" , config.Ki_z , 0.73);
        n_priv.param<double>("ki_w" , config.Ki_w , 0.74);
        n_priv.param<double>("kd_x" , config.Kd_x , 0.71);
        n_priv.param<double>("kd_y" , config.Kd_y , 0.72);
        n_priv.param<double>("kd_z" , config.Kd_z , 0.73);
        n_priv.param<double>("kd_w" , config.Kd_w , 0.74);
        config.Load_PID = 0;
    }
    if (config.PID_Control)
        control_gui_enable_ = true;
    else
        control_gui_enable_ = false;

    if (config.Take_off)
    {
        std_msgs::Empty msg;
        takeoff_pub.publish(msg);
        config.Take_off    = 0;
    }
    if (config.Land)
    {
        std_msgs::Empty msg;
        land_pub.publish(msg);
        config.Land    = 0;
    }
    if (config.Reset)
    {
        std_msgs::Empty msg;
        reset_pub.publish(msg);
        config.Reset    = 0;
    }
    if (config.Send_Position)
    {
        goal_pose_ <<   config.X_Position,
                config.Y_Position,
                config.Z_Position,
                0,
                0,
                0;
        config.Send_Position = 0;
        ROS_INFO("GOT NEW GOAL FROM THE GUI");
        std::cout << "goal_pose: " << goal_pose_.transpose()<< std::endl;
    }

    Kp << config.Kp_x , config.Kp_y , config.Kp_z , 0 , 0, config.Kp_w;
    Ki << config.Ki_x , config.Ki_y , config.Ki_z , 0 , 0, config.Ki_w;
    Kd << config.Kd_x , config.Kd_y , config.Kd_z , 0 , 0, config.Kd_w;

    control_info_msg.Kp.x = config.Kp_x ;
    control_info_msg.Kp.y = config.Kp_y ;
    control_info_msg.Kp.z = config.Kp_z ;
    control_info_msg.Kp.w = config.Kp_w ;

    control_info_msg.Ki.x = config.Ki_x ;
    control_info_msg.Ki.y = config.Ki_y ;
    control_info_msg.Ki.z = config.Ki_z ;
    control_info_msg.Ki.w = config.Ki_w ;

    control_info_msg.Kd.x = config.Kd_x ;
    control_info_msg.Kd.y = config.Kd_y ;
    control_info_msg.Kd.z = config.Kd_z ;
    control_info_msg.Kd.w = config.Kd_w ;
}
void PositionCommand::goalCB()
{
    geometry_msgs::PoseStamped Goal;
    Goal = as_.acceptNewGoal()->goal;
    Eigen::Matrix<double,3,1> euler = Eigen::Quaterniond(Goal.pose.orientation.w,
                                                         Goal.pose.orientation.x,
                                                         Goal.pose.orientation.y,
                                                         Goal.pose.orientation.z).matrix().eulerAngles(2, 1, 0);
    double yaw   = euler(0,0);
    double pitch = euler(1,0);
    double roll  = euler(2,0);

    goal_pose_ <<   Goal.pose.position.x,
            Goal.pose.position.y,
            Goal.pose.position.z,
            roll,
            pitch,
            yaw;

    ROS_INFO("GOT NEW GOAL from Action client");
    std::cout << "goal_pose: " << goal_pose_.transpose()<< std::endl;

    action_recived_ = true;
    return;
}
double Sat (double num, double Max , double Min){
    if (num > Max)      {num = Max;}
    else if (num < Min) {num = Min;}
    return num;
}
bool error(const Eigen::Matrix<double,6,1> Pose, const Eigen::Matrix<double,6,1> Target)
{
    double x,y,z,e;
    x = Pose(0,0)-Target(0,0);
    y = Pose(1,0)-Target(1,0);
    z = Pose(2,0)-Target(2,0);
    e = sqrt(x*x+y*y+z*z);
    if (e < EPS)
        return true;
    else
        return false;
}



int main(int argc, char **argv)
{
    ros::init(argc, argv, "uav_commander");
    ros::NodeHandle n;

    ros::Rate       loop_rate(40);
    PositionCommand position_commander(n,ros::this_node::getName());

    while (ros::ok())
    {
        position_commander.control();

        ros::spinOnce();
        loop_rate.sleep();
    }

    return 0;
}

