#ifndef _MAP_MATCH_HPP_
#define _MAP_MATCH_HPP_

#include<iostream>
#include<ros/ros.h>
#include<vector>

#include<sensor_msgs/PointCloud2.h>
#include<nav_msgs/Odometry.h>

#include<tf/transform_broadcaster.h>

#include<pcl/io/pcd_io.h>
#include<pcl/registration/ndt.h>
#include<pcl/filters/approximate_voxel_grid.h>
#include<pcl/filters/voxel_grid.h>
#include<pcl_conversions/pcl_conversions.h>
// #include<pcl/ros/conversions.h>
#include<pcl/point_cloud.h>



class Matcher{

    private:
        ros::Publisher pc_pub;
        ros::Publisher map_pub;
        ros::Publisher odom_pub;

        ros::Subscriber pc_sub;
        ros::Subscriber odom_sub;

        pcl::PointCloud<pcl::PointXYZI>::Ptr local_lidar_cloud;
        pcl::PointCloud<pcl::PointXYZI>::Ptr map_cloud;
        pcl::PointCloud<pcl::PointXYZI>::Ptr local_map_cloud;
        pcl::PointCloud<pcl::PointXYZI>::Ptr ndt_cloud;
        pcl::NormalDistributionsTransform<pcl::PointXYZI, pcl::PointXYZI> ndt;//default value

        std::string PARENT_FRAME, CHILD_FRAME;
        double VOXEL_SIZE, LIMIT_RANGE;
        double MATCHING_SCORE_THRESHOLD;
        double CLOUD_MAP_OFFSET_X;
        double CLOUD_MAP_OFFSET_Y;
        double CLOUD_MAP_OFFSET_Z;
        double CLOUD_MAP_OFFSET_ROLL;
        double CLOUD_MAP_OFFSET_PITCH;
        double CLOUD_MAP_OFFSET_YAW;
        double RESOLUTION;

        ros::Time buffer_time;
        sensor_msgs::PointCloud2 buffer_pc;
        nav_msgs::Odometry buffer_odom;

        Eigen::Matrix4f ndt_matching(
                pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_tgt,
                pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_src,
                pcl::PointCloud<pcl::PointXYZI>::Ptr &cloud, nav_msgs::Odometry odo);


        void local_pc(
                pcl::PointCloud<pcl::PointXYZI>::Ptr input_cloud,
                pcl::PointCloud<pcl::PointXYZI>::Ptr& output_cloud,
                double x_now,double y_now);


        void calc_rpy(Eigen::Matrix4f ans, double &yaw);

    public:
        Matcher(ros::NodeHandle n,ros::NodeHandle priv_nh);
        void map_read(std::string filename);
        void lidarcallback(const sensor_msgs::PointCloud2::Ptr msg);
        void odomcallback(const nav_msgs::OdometryConstPtr& msg);
        void process();

        bool is_start;
};

#endif

