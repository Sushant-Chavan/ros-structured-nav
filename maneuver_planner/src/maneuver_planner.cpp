/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2018, TU/e
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
*   * Neither the name of Willow Garage, Inc. nor the names of its
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
* Authors: Cesar lopez
*********************************************************************/
#include <maneuver_planner/maneuver_planner.h>
#include <pluginlib/class_list_macros.h>

//register this planner as a BaseGlobalPlanner plugin
PLUGINLIB_EXPORT_CLASS(maneuver_planner::ManeuverPlanner, nav_core::BaseGlobalPlanner)

namespace maneuver_planner {

ManeuverPlanner::ManeuverPlanner()
    : costmap_ros_(NULL), initialized_(false)
{}

ManeuverPlanner::ManeuverPlanner(std::string name, costmap_2d::Costmap2DROS* costmap_ros)
    : costmap_ros_(NULL), initialized_(false)
{
    initialize(name, costmap_ros);
}

void ManeuverPlanner::initialize(std::string name, costmap_2d::Costmap2DROS* costmap_ros)
{
    if(!initialized_)
    {
        costmap_ros_ = costmap_ros;
        costmap_ = costmap_ros_->getCostmap();

        ros::NodeHandle private_nh("~/" + name);
        private_nh.param("step_size", step_size_, costmap_->getResolution());
        private_nh.param("min_dist_from_robot", min_dist_from_robot_, 0.10);
        private_nh.param("turning_radius", turning_radius_, 0.8);
        private_nh.param("use_last_goal_as_start", last_goal_as_start_, false);
        valid_last_goal_ = false;
        world_model_ = new base_local_planner::CostmapModel(*costmap_);


        // For now only rectangular robot shape is supported. Initiallize transformation matrices
        std::vector<geometry_msgs::Point> footprint = costmap_ros_->getRobotFootprint();
        //if we have no footprint... do nothing
        if(footprint.size() != 4)
        {
            ROS_ERROR("Footprint must have hour points");
            return;
        }

        // The heading of the robot is towards the x axis.
        bool topRightCorner_b = false;
        bool topLeftCorner_b = false;
        bool bottomRightCorner_b = false;
        bool bottomLeftCorner_b = false;



        for (int i_fp = 0; i_fp<footprint.size(); i_fp++)
        {
// 	ROS_INFO("footprint %.3f, %.3f", footprint[i_fp].x, footprint[i_fp].y);
            if( footprint[i_fp].x > 0 && footprint[i_fp].y > 0)
            {
                topLeftCorner_b = true;
                topLeftCorner_ << footprint[i_fp].x << footprint[i_fp].y << 0.0 << arma::endr;
            } 
            else if( footprint[i_fp].x > 0 && footprint[i_fp].y < 0)
            {
                topRightCorner_b = true;
                topRightCorner_ << footprint[i_fp].x << footprint[i_fp].y << 0.0 << arma::endr;
            } 
            else if( footprint[i_fp].x < 0 && footprint[i_fp].y < 0)
            {
                bottomRightCorner_b = true;
                bottomRightCorner_ << footprint[i_fp].x << footprint[i_fp].y << 0.0 << arma::endr;
            } 
            else if( footprint[i_fp].x < 0 && footprint[i_fp].y > 0)
            {
                bottomLeftCorner_b = true;
                bottomLeftCorner_ << footprint[i_fp].x << footprint[i_fp].y << 0.0 << arma::endr;
            }
        }

        if( ! (topRightCorner_b & topLeftCorner_b & bottomRightCorner_b & bottomLeftCorner_b) )
        {
            ROS_ERROR("Footprint must have four corners and center of rotation inside the footprint");
            return;
        }
        else
        {
            jacobian_topRightCorner_  << 1.0 << -topRightCorner_[1] << arma::endr
                                      << 0.0 <<  topRightCorner_[0] << arma::endr;

            jacobian_topLeftCorner_   << 1.0 <<  -topLeftCorner_[1] << arma::endr
                                      << 0.0 <<   topLeftCorner_[0] << arma::endr;

            jacobian_bottomLeftCorner_<< 1.0 <<  -bottomLeftCorner_[1] << arma::endr
                                      << 0.0 <<   bottomLeftCorner_[0] << arma::endr;

            jacobian_bottomRightCorner_<< 1.0 <<  -bottomRightCorner_[1] << arma::endr
                                       << 0.0 <<   bottomRightCorner_[0] << arma::endr;
        }
        left_side_ref_point_ << 0.1 << bottomLeftCorner_[1] << 0.0 << arma::endr; // 0.1 above center of rotation worked well in simulations. This point will be fixed
        right_side_ref_point_ << 0.1 << bottomRightCorner_[1] << 0.0 << arma::endr; // 0.1 above center of rotation worked well in simulations. This point will be fixed



        initialized_ = true;
    }
    else
        ROS_WARN("This planner has already been initialized... doing nothing");
}

void ManeuverPlanner::rotate2D(const tf::Stamped<tf::Pose> &pose_tf_in, const double theta, tf::Stamped<tf::Pose> &pose_tf_out)
{
    tf::Vector3 origin;
    tf::Quaternion quat;
    quat.setRPY(0.0,0.0,theta);
    double ty,tp,tr;
    origin = pose_tf_in.getOrigin().rotate(tf::Vector3(0,0,1),theta);
    pose_tf_in.getBasis().getEulerYPR(ty,tp,tr);
    quat.setRPY(0.0,0.0,theta+ty);
    pose_tf_out.setData(tf::Transform(quat,origin));
    pose_tf_out.stamp_ = pose_tf_in.stamp_;

    return;

//     arma::mat vec_out;
//     arma::mat rotM;
//     rotM  << cos(theta) <<  -sin(theta)  << arma::endr
// 	  << sin(theta) <<   cos(theta)  << arma::endr;
//     vec_out = rotM*vec_in.in_range( arma::span(0, 1) );
//     vec_out[3] = vec_in[3] + theta;
//     return  vec_out;
}

void ManeuverPlanner::translate2D(const tf::Stamped<tf::Pose>& pose_tf_in, const tf::Vector3& vector3_translation, tf::Stamped<tf::Pose> &pose_tf_out)
{

    tf::Vector3 origin;
    tf::Quaternion quat;

    origin = pose_tf_in.getOrigin() + vector3_translation;
    quat = pose_tf_in.getRotation();
    pose_tf_out.setData(tf::Transform(quat,origin));
    pose_tf_out.stamp_ = pose_tf_in.stamp_;

//     arma::mat vec_out;
//     vec_out = vec_in.in_range( arma::span(0, 1) ) + vec_tr.in_range( arma::span(0, 1) );
//     vec_out[3] = vec_in[3];
//     return  vec_out;
}


int  ManeuverPlanner::computeCurveParameters(const tf::Stamped<tf::Pose>& pose_target, const double& turning_radius, double &dist_before_steering, double &dist_after_steering, double &signed_turning_radius)
{
    int curve_type = ManeuverPlanner::NONE;
    dist_before_steering   = -1.0;
    dist_after_steering    = -1.0;
    signed_turning_radius  =  0.0;
    double x_target   = pose_target.getOrigin().getX();
    double y_target   = pose_target.getOrigin().getY();
    double yaw_target,useless_pitcht,useless_rollt;
    pose_target.getBasis().getEulerYPR(yaw_target,useless_pitcht,useless_rollt);

    // the frame reference is at the starting position of the reference point, thus at the origin

    // By definition the intersection lies in the y-axis at (x_intersection,0.0)
    double x_intersection = x_target - y_target/std::tan(yaw_target); // derived from geometry relations

    if( x_intersection < 0 )
    {
        ROS_WARN("No single turn possible, xi<0. Try multiple turns maneouver");
        return ManeuverPlanner::NONE;
    }


    double dist_target_to_intersection = std::sqrt( (x_target-x_intersection)*(x_target-x_intersection) + y_target*y_target );
    double maximum_turning_radius = std::min(std::abs(x_intersection),std::abs(dist_target_to_intersection));

    // for left turn signed_turning_radius>0, for right signed_turning_radius rs<0
    if (y_target > 0)
    {
        if(yaw_target < 0.0 | yaw_target > M_PI)
        {
            ROS_WARN("Target on the left but orientation is facing to the right");
            return ManeuverPlanner::NONE;
        }
        else
        {
            curve_type = ManeuverPlanner::LEFT_CENTER_POINT;
            signed_turning_radius = std::abs(turning_radius);
        }
    }
    else
    {
        if(yaw_target > 0.0 | yaw_target < -M_PI)
        {
            ROS_WARN("Target on the right but orientation is facing to the left");
            return ManeuverPlanner::NONE;
        }
        else
        {
            curve_type = ManeuverPlanner::RIGHT_CENTER_POINT;
            signed_turning_radius = -std::abs(turning_radius);
        }
    }

    double dist_x_intersection_steering = signed_turning_radius/tan((M_PI-yaw_target)/2.0);
    dist_before_steering =  x_intersection - dist_x_intersection_steering;
    dist_after_steering = dist_target_to_intersection - dist_x_intersection_steering;

    if ( dist_before_steering<0.0 |  dist_after_steering<0.0 | dist_before_steering > x_intersection )
    {
        ROS_WARN("No single turn possible with desired radius, dist_bs<0 || dist_as<0 || dist_bs>xi. Change turning radius or try multiple turns maneouver");
        return ManeuverPlanner::NONE;
    }


    return curve_type;
}

//we need to take the footprint of the robot into account when we calculate cost to obstacles
double ManeuverPlanner::footprintCost(double x_i, double y_i, double theta_i)
{
    if(!initialized_)
    {
        ROS_ERROR("The planner has not been initialized, please call initialize() to use the planner");
        return -1.0;
    }

    std::vector<geometry_msgs::Point> footprint = costmap_ros_->getRobotFootprint();

    //if we have no footprint... do nothing
    if(footprint.size() < 3)
        return -1.0;

    //check if the footprint is legal
    double footprint_cost = world_model_->footprintCost(x_i, y_i, theta_i, footprint);
    return footprint_cost;
}


bool ManeuverPlanner::makePlan(const geometry_msgs::PoseStamped& start,
                               const geometry_msgs::PoseStamped& goal, std::vector<geometry_msgs::PoseStamped>& plan)
{

    if(!initialized_)
    {
        ROS_ERROR("The planner has not been initialized, please call initialize() to use the planner");
        return false;
    }

    ROS_DEBUG("Got a start: %.2f, %.2f, and a goal: %.2f, %.2f", start.pose.position.x, start.pose.position.y, goal.pose.position.x, goal.pose.position.y);

    plan.clear();
    costmap_ = costmap_ros_->getCostmap();

    if(goal.header.frame_id != costmap_ros_->getGlobalFrameID())
    {
        ROS_ERROR("This planner as configured will only accept goals in the %s frame, but a goal was sent in the %s frame.",
                  costmap_ros_->getGlobalFrameID().c_str(), goal.header.frame_id.c_str());
        return false;
    }

    tf::Stamped<tf::Pose> goal_tf;
    tf::Stamped<tf::Pose> start_tf;
    double useless_pitch, useless_roll, goal_yaw, start_yaw;

    /*
        poseStampedMsgToTF(goal,goal_tf);
        poseStampedMsgToTF(start,start_tf);

        start_tf.getBasis().getEulerYPR(start_yaw, useless_pitch, useless_roll);
        goal_tf.getBasis().getEulerYPR(goal_yaw, useless_pitch, useless_roll);
       */
    /****************************/
    double temp_yaw, temp_pitch, temp_roll;
    tf::Quaternion temp_quat;
    tf::Vector3 temp_vector3;

    if( last_goal_as_start_ & valid_last_goal_)
    {
        start_ = last_goal_;
    }
    else
    {
        start_ = start;
    }

    poseStampedMsgToTF(goal,goal_tf);
    poseStampedMsgToTF(start_,start_tf);


//     temp_quat.setRPY(0.0,0.0,1.5708);
//     temp_vector3 = tf::Vector3(1.19, -1.36, 0.0);
//     start_tf.setData(tf::Transform(temp_quat,temp_vector3));
//     temp_quat.setRPY(0.0,0.0,3.1416 );
//     temp_vector3 = tf::Vector3(-0.11, 1.19, 0.0);
//     goal_tf.setData(tf::Transform(temp_quat,temp_vector3));

    start_tf.getBasis().getEulerYPR(start_yaw, useless_pitch, useless_roll);
    goal_tf.getBasis().getEulerYPR(goal_yaw, useless_pitch, useless_roll);


    tf::Stamped<tf::Pose> goal_tf_start_coord; // Goal in the start vector coordinates
    goal_tf_start_coord.frame_id_ = "/center_rotation_start_pos";

    translate2D(goal_tf,-start_tf.getOrigin(),goal_tf_start_coord);
    rotate2D(goal_tf_start_coord,-start_yaw,goal_tf_start_coord);




    // Compute parameters for left or right turn using center of rotation
    double dist_before_steering_center, dist_after_steering_center, signed_turning_radius_center;
    int curve_type = computeCurveParameters(goal_tf_start_coord, turning_radius_, dist_before_steering_center, dist_after_steering_center, signed_turning_radius_center);
//     ROS_INFO("Curve parameters: %.3f / %.3f / %.3f",dist_before_steering, dist_after_steering, signed_turning_radius);

    tf::Stamped<tf::Pose> refpoint_goal_tf; // Goal Refpoint in the global coordinate frame
    tf::Stamped<tf::Pose> refpoint_start_tf; // Start Refpoint in the global coordinate frame
    tf::Stamped<tf::Pose> refpoint_goal_tf_refstart_coord; // Goal Refpoint in the the start position of the reference point coordinate frame
    tf::Stamped<tf::Pose> refpoint_tf_robot_coord; // Refpoint in the robot(+load) coordinate frame
    double dist_before_steering_refp, dist_after_steering_refp, signed_turning_radius_refp;
    int curve_type_2step;

    if (curve_type != ManeuverPlanner::NONE)
    {
        switch (curve_type)
        {
        case ManeuverPlanner::LEFT_CENTER_POINT :
            // Initially choose top right corner (trc) as reference to generate
            // trajectory. Suitable for when trc is desired to keep parallel to the
            // wall. If the trc trajectory is not convex, we switch back
            // reference point to middle of the rotation axis.
            refpoint_tf_robot_coord.frame_id_ = "/wholerobot_link";
            refpoint_tf_robot_coord.stamp_ = goal_tf.stamp_;
            temp_quat.setRPY(0.0,0.0,0.0);
            temp_vector3 = tf::Vector3(topRightCorner_[0], topRightCorner_[1], 0.0);
            refpoint_tf_robot_coord.setData(tf::Transform(temp_quat,temp_vector3));
            ROS_INFO("Left turn");
            break;

        case ManeuverPlanner::RIGHT_CENTER_POINT :
            // Choose a point on the right side, just above axis of rotation
            // as reference to generate trajectory
            // Initially choose right side as reference to generate
            // trajectory. Suitable for when that point should just round the corner.
            // If recomputing trajectory is not possible, the switch back
            // reference point to middle of the rotation axis.
            refpoint_tf_robot_coord.frame_id_ = "/wholerobot_link";
            refpoint_tf_robot_coord.stamp_ = goal_tf.stamp_;
            temp_quat.setRPY(0.0,0.0,0.0);
            temp_vector3 = tf::Vector3(right_side_ref_point_[0], right_side_ref_point_[1], 0.0);
            refpoint_tf_robot_coord.setData(tf::Transform(temp_quat,temp_vector3));
            ROS_INFO("Right turn");
            break;
        default:
            break;
        }


        /*** Based on the selected reference point, re-compute new possible maneuver. ****/


        // Compute reference start and goal on global coordinates
        refpoint_start_tf.frame_id_ = goal_tf.frame_id_;
        refpoint_start_tf.stamp_ = goal_tf.stamp_;
        rotate2D(refpoint_tf_robot_coord,start_yaw,refpoint_start_tf);
        translate2D(refpoint_start_tf,start_tf.getOrigin(),refpoint_start_tf);

        refpoint_goal_tf.frame_id_ = goal_tf.frame_id_;
        refpoint_goal_tf.stamp_ = goal_tf.stamp_;
        rotate2D(refpoint_tf_robot_coord,goal_yaw,refpoint_goal_tf);
        translate2D(refpoint_goal_tf,goal_tf.getOrigin(),refpoint_goal_tf);

        // Then Compute reference start and goal on reference start coordinates
        refpoint_goal_tf_refstart_coord.frame_id_ = "/refpoint_start_pos";
        refpoint_goal_tf_refstart_coord.stamp_ = goal_tf.stamp_;

        translate2D(refpoint_goal_tf,-refpoint_start_tf.getOrigin(),refpoint_goal_tf_refstart_coord);
        refpoint_start_tf.getBasis().getEulerYPR(temp_yaw, temp_pitch, temp_roll);
        rotate2D(refpoint_goal_tf_refstart_coord,-temp_yaw,refpoint_goal_tf_refstart_coord);

        curve_type_2step = computeCurveParameters(refpoint_goal_tf_refstart_coord, turning_radius_, dist_before_steering_refp, dist_after_steering_refp, signed_turning_radius_refp);
        // 	ROS_INFO("Curve parameters: %.3f / %.3f / %.3f",dist_before_steering_refp, dist_after_steering_refp, signed_turning_radius_refp);
        if (curve_type_2step == ManeuverPlanner::NONE)
        {
            // Come back to reference point at the center
            ROS_INFO("Setting reference point back to center of rotation");
            refpoint_tf_robot_coord.frame_id_ = "/wholerobot_link";
            refpoint_tf_robot_coord.stamp_ = goal_tf.stamp_;
            temp_quat.setRPY(0.0,0.0,0.0);
            temp_vector3 = tf::Vector3(0.0,0.0, 0.0);
            refpoint_tf_robot_coord.setData(tf::Transform(temp_quat,temp_vector3));
            dist_before_steering_refp 	= dist_before_steering_center;
            dist_after_steering_refp 	= dist_after_steering_center;
            signed_turning_radius_refp    = signed_turning_radius_center;
            refpoint_goal_tf_refstart_coord = goal_tf_start_coord;
        }
        else
        {
            curve_type = curve_type_2step;
        }

    }


    /***************  Next, Generate trajectory ******************/

    double footprint_cost;
    int counter = 1;

    if(curve_type != ManeuverPlanner::NONE)
    {


        tf::Stamped<tf::Pose> ref_traj_point_tf_refstart_coord; // current  Refpoint in the the start position of the reference point coordinate frame
        tf::Stamped<tf::Pose> center_traj_point_tf_refstart_coord; // current  Center point in the the start position of the reference point coordinate frame
        tf::Stamped<tf::Pose> center_traj_point_tf; // current  Center point in the global coordinate frame

        // Initialize trajectory of reference point. By definition at the origin


        // Initialize trajectory of center point. By definition at -refpoint_tf_robot_coord
        center_traj_point_tf_refstart_coord.frame_id_ = "/refpoint_start_pos";
        center_traj_point_tf_refstart_coord.stamp_ = goal_tf.stamp_;
        temp_quat.setRPY(0.0,0.0,0.0);
        temp_vector3 = -refpoint_tf_robot_coord.getOrigin();
        center_traj_point_tf_refstart_coord.setData(tf::Transform(temp_quat,temp_vector3));
        // This pose as a vector because it will be more direct to make operations on it. x y theta
        center_pose_loctrajframe_ << temp_vector3.getX() << temp_vector3.getY() << 0.0 << arma::endr;


        // Compute global coordinates center trajectory point
        center_traj_point_tf.frame_id_ = goal_tf.frame_id_;
        center_traj_point_tf.stamp_ = goal_tf.stamp_;

        translate2D(center_traj_point_tf_refstart_coord,refpoint_tf_robot_coord.getOrigin(),center_traj_point_tf);
        rotate2D(center_traj_point_tf,start_yaw,center_traj_point_tf);
        translate2D(center_traj_point_tf,start_tf.getOrigin(),center_traj_point_tf);

        // Compute trajectory as points. Only the center of teh robot has a pose and orientation
        motion_refpoint_localtraj_ << 0.0 << arma::endr
                                   << 0.0 << arma::endr ; // starts at origin by definition
        prev_motion_refpoint_localtraj_ = motion_refpoint_localtraj_;

        double theta_refp_goal; // Final angle of curvature
        refpoint_goal_tf_refstart_coord.getBasis().getEulerYPR(theta_refp_goal, temp_pitch, temp_roll);
        double theta_refp_traj = 0.0 ; // This is only to control evolution of the curvature.
        double theta_refp_traj_gridsz = step_size_/signed_turning_radius_refp; // Gridsize of the trajectory angle
        double dist_bef_steer = 0.0, dist_af_steer = 0.0;
        // Jacobian to compute virtual velocities and therefore positions.
        jacobian_motrefPoint_ << 1.0 << -refpoint_tf_robot_coord.getOrigin().getY() << arma::endr
                              << 0.0 <<  refpoint_tf_robot_coord.getOrigin().getX() << arma::endr;
        arma::mat invjacobian_motrefPoint;

        if ( refpoint_tf_robot_coord.getOrigin().getX() != 0.0 )
        {   // Check refpoint is not at the center
            invjacobian_motrefPoint = arma::inv(jacobian_motrefPoint_);
        }
        else
        {
            invjacobian_motrefPoint << 0.0 << 0.0 << arma::endr
                                    << 0.0 << 0.0 << arma::endr;
        }


        bool traj_ready = false;
        bool traj_free = true;


//     ROS_ERROR("Reached here %d", counter);counter++;

        while (!traj_ready)
        {
//       counter++;
            // Check for obstacles of last computed traj pose
            center_traj_point_tf.getBasis().getEulerYPR(temp_yaw, temp_pitch, temp_roll);
            temp_vector3 = center_traj_point_tf.getOrigin();
            footprint_cost = footprintCost(temp_vector3.getX(), temp_vector3.getY(), temp_yaw);
            if(footprint_cost < 0)
            {
                // Abort. Inform that not all trajectory is free of obstcales
                traj_ready = true;
                traj_free  = false;
            }
            else
            {   // Add current point to overall trajectory
                geometry_msgs::PoseStamped traj_point = goal;
// 	poseStampedTFToMsg(center_traj_point_tf,traj_point);
                tf::Quaternion goal_quat = tf::createQuaternionFromYaw(temp_yaw);

                traj_point.pose.position.x = temp_vector3.getX();
                traj_point.pose.position.y = temp_vector3.getY();

                traj_point.pose.orientation.x = goal_quat.x();
                traj_point.pose.orientation.y = goal_quat.y();
                traj_point.pose.orientation.z = goal_quat.z();
                traj_point.pose.orientation.w = goal_quat.w();

                plan.push_back(traj_point);

            }
            // Generate next reference refpoint in trajectory
            if( dist_bef_steer <  dist_before_steering_refp)
            {   // Move straight before steering
                theta_refp_traj = 0;
                motion_refpoint_localtraj_[0] = prev_motion_refpoint_localtraj_[0]+ step_size_;
                motion_refpoint_localtraj_[1] = prev_motion_refpoint_localtraj_[1];
                dist_bef_steer += step_size_;
            } 
            else if(std::abs(theta_refp_goal-theta_refp_traj) > std::abs(theta_refp_traj_gridsz/2.0))
            {   // Turn with circle. This can be as well a clothoid!
                theta_refp_traj +=theta_refp_traj_gridsz;
                motion_refpoint_localtraj_[0] = dist_before_steering_refp +signed_turning_radius_refp*std::sin(theta_refp_traj);
                motion_refpoint_localtraj_[1] = signed_turning_radius_refp*(1.0 - std::cos(theta_refp_traj));
            } 
            else if( dist_af_steer <  dist_after_steering_refp)
            {   // Move straight after steering
                theta_refp_traj = theta_refp_goal;
                motion_refpoint_localtraj_[0] = prev_motion_refpoint_localtraj_[0]+ step_size_*std::cos(theta_refp_traj);
                motion_refpoint_localtraj_[1] = prev_motion_refpoint_localtraj_[1]+ step_size_*std::sin(theta_refp_traj);
                dist_af_steer += step_size_;
            }
            else
            {
                traj_ready = true;
                break;
            }


            // Now compute robot center of rotation trajectory
            // Compute virtual velocity of reference point. Virtual time of 1.0 sec
            motion_refpoint_virvel_loctrajframe_ = (motion_refpoint_localtraj_ - prev_motion_refpoint_localtraj_)/1.0;
            prev_motion_refpoint_localtraj_ = motion_refpoint_localtraj_;

            if ( refpoint_tf_robot_coord.getOrigin().getX() != 0.0 )
            {   // Check refpoint is not at the center
                //Compute center of rotation pose from inverse Jacobian
                arma::mat RotM;
                RotM    <<  std::cos(center_pose_loctrajframe_[2]) <<  std::sin(center_pose_loctrajframe_[2]) << arma::endr
                        << -std::sin(center_pose_loctrajframe_[2]) <<  std::cos(center_pose_loctrajframe_[2]) << arma::endr;
                // Compute refpoint velocity local at the robot by rotating velocity vector
                motion_refpoint_virvel_robotframe_ = RotM*motion_refpoint_virvel_loctrajframe_;
                // Compute the corresponding robot velocity using inverse of the jacobian
                center_vel_robotframe_ = invjacobian_motrefPoint*motion_refpoint_virvel_robotframe_; // [dx dtheta]
                // Compute evolution of the robot by integrating virtual velocity (dt virtual is 1.0 sec)
                center_pose_loctrajframe_[2] += 1.0*center_vel_robotframe_[1];
                center_pose_loctrajframe_[0] += 1.0*center_vel_robotframe_[0]*std::cos(center_pose_loctrajframe_[2]);
                center_pose_loctrajframe_[1] += 1.0*center_vel_robotframe_[0]*std::sin(center_pose_loctrajframe_[2]);
            }
            else
            {
                // Compute center of rotation directly from ref_point positions
                center_pose_loctrajframe_[0] = motion_refpoint_localtraj_[0];
                center_pose_loctrajframe_[1] = motion_refpoint_localtraj_[1];
                center_pose_loctrajframe_[2] = theta_refp_traj;

            }

            // Center pose in local trajectory frame, in tf::PoseStamped
            temp_quat.setRPY(0.0,0.0,center_pose_loctrajframe_[2]);
            temp_vector3 = tf::Vector3(center_pose_loctrajframe_[0],center_pose_loctrajframe_[1],0.0);
            center_traj_point_tf_refstart_coord.setData(tf::Transform(temp_quat,temp_vector3));

            // Center pose in global frame, in tf::PoseStamped
            translate2D(center_traj_point_tf_refstart_coord,refpoint_tf_robot_coord.getOrigin(),center_traj_point_tf);
            rotate2D(center_traj_point_tf,start_yaw,center_traj_point_tf);
            translate2D(center_traj_point_tf,start_tf.getOrigin(),center_traj_point_tf);


            /*if(counter >= 17 && counter <= 20)
            {
                center_traj_point_tf.getBasis().getEulerYPR(temp_yaw,temp_pitch,temp_roll);
            ROS_INFO("center_traj_point_tf: x= %.3f y= %.3f theta= %.3f", center_traj_point_tf.getOrigin().getX(), center_traj_point_tf.getOrigin().getY(), temp_yaw);
            ROS_INFO("center_pose_loctrajframe_: %.4f / %.4f / %.4f",center_pose_loctrajframe_[0], center_pose_loctrajframe_[1],center_pose_loctrajframe_[2]);
            ROS_INFO("center_vel_robotframe_: %.4f / %.4f / %.4f",center_vel_robotframe_[0], center_vel_robotframe_[1],center_vel_robotframe_[2]);

            }*/
        }


    }
    else
    {

        ROS_WARN("No single left or right maneuver possible. Execute carrot planner");

        /***** Own Carrot planner ****/

        //we want to step forward along the vector created by the robot's position and the goal pose until we find an illegal cell
        double goal_x = goal.pose.position.x;
        double goal_y = goal.pose.position.y;
        double start_x = start.pose.position.x;
        double start_y = start.pose.position.y;

        double diff_x = goal_x - start_x;
        double diff_y = goal_y - start_y;
        double diff_yaw = angles::normalize_angle(goal_yaw-start_yaw);

        double target_x = start_x;
        double target_y = start_y;
        double target_yaw = goal_yaw;

        bool done = false;
        double scale = 0.0;
        double dScale = 0.05;

        while(!done)
        {
            if(scale > 1.0)
            {
                target_x = start_x;
                target_y = start_y;
                target_yaw = start_yaw;

                done = true;
                break;
            }
            target_x = start_x + scale * diff_x;
            target_y = start_y + scale * diff_y;
            target_yaw = angles::normalize_angle(start_yaw + scale * diff_yaw);

            footprint_cost = footprintCost(target_x, target_y, target_yaw);
            if(footprint_cost < 0)
            {
                done = true;
                break;
            }
            scale +=dScale;

            geometry_msgs::PoseStamped new_goal = goal;
            tf::Quaternion goal_quat = tf::createQuaternionFromYaw(target_yaw);

            new_goal.pose.position.x = target_x;
            new_goal.pose.position.y = target_y;

            new_goal.pose.orientation.x = goal_quat.x();
            new_goal.pose.orientation.y = goal_quat.y();
            new_goal.pose.orientation.z = goal_quat.z();
            new_goal.pose.orientation.w = goal_quat.w();

            plan.push_back(new_goal);

        }
        if(scale == 0.0)
        {
            ROS_WARN("The maneouver planner could not find a valid plan for this goal");
        }

        /********************************/



    }
    return true;

}

};