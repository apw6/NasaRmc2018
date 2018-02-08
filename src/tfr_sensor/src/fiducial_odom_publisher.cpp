/**
 * Fiducial odometry publisher, currently it's a quick and dirty test class to
 * get sensor fusion, and navigation up and running. 
 *
 * If the proof of concept is reliable in any way, we will refactor to a more
 * maintainable form. 
 *
 * Functionally it subscribes to a camera topic, feeds that to the fiducial
 * action server, and publishes the relevant Odometry information at a supplied
 * camera_link frame.
 *
 * It only publishes odometry if the fiducial action server is successful.
 *
 * parameters:
 *   ~camera_frame: The reference frame of the camera (string, default="camera_link")
 *   ~footprint_frame: The reference frame of the robot_footprint(string,
 *   default="footprint")
 *   ~bin_frame: The reference frame of the bin (string, default="bin_link")
 *   ~odom_frame: The reference frame of odom  (string, default="odom")
 * subscribed topics:
 *   image (sensor_msgs/Image) - the camera topic
 * published topics:
 *   odom (geometry_msgs/Odometry)- the odometry topic 
 * */
#include <ros/ros.h>
#include <ros/console.h>
#include <image_transport/image_transport.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/PoseStamped.h>
#include <tfr_msgs/ArucoAction.h>
#include <actionlib/client/simple_action_client.h>
#include <tf2/convert.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2/LinearMath/Scalar.h>
#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/transform_listener.h>

class FiducialOdom
{
    public:
        FiducialOdom(ros::NodeHandle& n, 
                const std::string& c_frame, 
                const std::string& f_frame, 
                const std::string& b_frame,
                const std::string& o_frame) :
            client{"aruco_action_server", true},
            camera_frame{c_frame},
            footprint_frame{f_frame},
            bin_frame{b_frame},
            odometry_frame{o_frame},
            last_pose{}
        {
            tf2_ros::TransformListener listener(tf_buffer);
            image_transport::ImageTransport it{n};
            subscriber = it.subscribeCamera("image",10, 
                    &FiducialOdom::process_odometry, this);
            publisher = n.advertise<nav_msgs::Odometry>("odom", 10 );
            ROS_INFO("Fiducial Odom Publisher Connecting to Server");
            client.waitForServer();
            ROS_INFO("Fiducial Odom Publisher Connected to Server");
            //fill transform buffer
            ros::Duration(2).sleep();
        }
        ~FiducialOdom() = default;
        FiducialOdom(const FiducialOdom&) = delete;
        FiducialOdom& operator=(const FiducialOdom&) = delete;
        FiducialOdom(FiducialOdom&&) = delete;
        FiducialOdom& operator=(FiducialOdom&&) = delete;

    private:
        void process_odometry(const sensor_msgs::ImageConstPtr& image, const
                sensor_msgs::CameraInfoConstPtr& info)
        {
            tfr_msgs::ArucoGoal goal;
            goal.image = *image;
            goal.camera_info = *info;
            client.sendGoal(goal);
            if (client.waitForResult())
            {
                /*we need to publish 2 things:
                 * 1. Transform for tf
                 * 2. Odometry information 
                 * However tf is weird so the first thing we will have to do is
                 * translate our current tf information to base_footprint, to
                 * not disrupt the tree structure of the transforms*/

                auto result = client.getResult();

                if (result->number_found == 0)
                    return;

                geometry_msgs::PoseStamped unprocessed_pose = result->relative_pose;
                geometry_msgs::TransformStamped transform_stamped;
                try{
                    transform_stamped =
                        tf_buffer.lookupTransform(camera_frame, footprint_frame,
                                ros::Time(0));
                }
                catch (tf2::TransformException &ex) {
                    ROS_WARN("%s",ex.what());
                    ros::Duration(1.0).sleep();
                    return;
                }

                geometry_msgs::PoseStamped relative_pose;
                tf2::doTransform(unprocessed_pose, relative_pose, transform_stamped);

                //NOTE this negative sign is needed to make the transform work,
                //I have no idea why
                relative_pose.header.stamp = ros::Time::now();
                relative_pose.pose.position.x *= -1;

                // 1. handle transforms for tf
                geometry_msgs::TransformStamped transform;
                transform.header.stamp = ros::Time::now();
                transform.header.frame_id = bin_frame;
                transform.child_frame_id = footprint_frame;
                transform.transform.translation.x = relative_pose.pose.position.x;
                transform.transform.translation.y = relative_pose.pose.position.y;
                transform.transform.translation.z = relative_pose.pose.position.z;
                transform.transform.rotation = relative_pose.pose.orientation;
                broadcaster.sendTransform(transform);
            
                // 2. handle odometry data
                nav_msgs::Odometry odom;
                odom.header.frame_id = bin_frame;
                odom.header.stamp = ros::Time::now();
                odom.child_frame_id = footprint_frame;
                //get our pose and fudge some covariances
                odom.pose.pose = relative_pose.pose;
                odom.pose.covariance = {  5e-3,   0,   0,   0,   0,   0,
                                             0,5e-3,   0,   0,   0,   0,
                                             0,   0,5e-3,   0,   0,   0,
                                             0,   0,   0,5e-3,   0,   0,
                                             0,   0,   0,   0,5e-3,   0,
                                             0,   0,   0,   0,   0,5e-3};


                //handle uninitialized data
                if (    last_pose.pose.orientation.x == 0 &&
                        last_pose.pose.orientation.y == 0 &&
                        last_pose.pose.orientation.z == 0 &&
                        last_pose.pose.orientation.w == 0)
                    last_pose.pose.orientation.w = 1;
                
                //velocities are harder, we need to take a diffence and do some
                //conversions
                tf2::Transform t_0{};
                tf2::convert(last_pose.pose, t_0);
                tf2::Transform t_1{};
                tf2::convert(relative_pose.pose, t_1);

                /* take fast difference to get linear and angular delta inbetween
                 * timestamps
                 * https://answers.ros.org/question/12654/relative-pose-between-two-tftransforms/
                 */
                auto deltas = t_0.inverseTimes(t_1);
                auto linear_deltas = deltas.getOrigin();
                auto angular_deltas = deltas.getRotation();
 
                //convert from quaternion to rpy for odom compatibility
                tf2::Matrix3x3 converter{};
                converter.setRotation(angular_deltas);
                tf2::Vector3 rpy_deltas{};
                converter.getRPY(rpy_deltas[0], rpy_deltas[1], rpy_deltas[2]); 

                const tf2Scalar delta_t{
                    relative_pose.header.stamp.toSec() - last_pose.header.stamp.toSec()};
 
                odom.twist.twist.linear =  tf2::toMsg(linear_deltas/delta_t);
                odom.twist.twist.angular = tf2::toMsg(rpy_deltas/delta_t);

                odom.twist.covariance = {  5e-3,   0,   0,   0,   0,   0,
                                              0,5e-3,   0,   0,   0,   0,
                                              0,   0,5e-3,   0,   0,   0,
                                              0,   0,   0,5e-3,   0,   0,
                                              0,   0,   0,   0,5e-3,   0,
                                              0,   0,   0,   0,   0,5e-3};

                //fire it off! and cleanup
                publisher.publish(odom);
                last_pose = relative_pose;
            }
            else
            {
                ROS_WARN("Fiducial action server failed.");
            }
        }

        image_transport::CameraSubscriber subscriber;
        ros::Publisher publisher;
        actionlib::SimpleActionClient<tfr_msgs::ArucoAction> client;
        tf2_ros::TransformBroadcaster broadcaster;
        tf2_ros::Buffer tf_buffer;

        geometry_msgs::PoseStamped last_pose;
        
        const std::string& camera_frame;
        const std::string& footprint_frame;
        const std::string& bin_frame;
        const std::string& odometry_frame;
};

int main(int argc, char** argv)
{
    ros::init(argc, argv, "fiducial_odom_publisher");
    ros::NodeHandle n{};

    std::string camera_frame, footprint_frame, bin_frame, odometry_frame;
    ros::param::param<std::string>("~camera_frame", camera_frame, "camera_link");
    ros::param::param<std::string>("~footprint_frame", footprint_frame, "footprint");
    ros::param::param<std::string>("~bin_frame", bin_frame, "bin_link");
    ros::param::param<std::string>("~odometry_frame", odometry_frame, "odom");

    FiducialOdom fiducial_odom{n, camera_frame,footprint_frame, bin_frame, odometry_frame};

    ros::spin();
    return 0;
}
