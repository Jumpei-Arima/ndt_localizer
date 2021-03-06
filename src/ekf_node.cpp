/* NDT_Dgauss_ekf.cpp
 * 2016.10.05
 *
 * author : Takashi Ienaga
 *
 * Distribution : rwrc16のD-GaussとNDTの拡張カルマンフィルタ(EKF)
 *                入力：Gyro-Odometry
 *                観測：D-Gauss
 *                      NDT
 * を参考に
 */


/*Library*/
#include <ros/ros.h>
#include <tf/tf.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <iostream>
#include <math.h>
#include <Eigen/Core>
#include <Eigen/LU>
#include <geometry_msgs/Quaternion.h>
#include <sensor_msgs/Imu.h>
#include "ndt_localizer/EKF.h"

/*msgs*/
#include <nav_msgs/Odometry.h>
#include <std_msgs/Bool.h>
#include <std_msgs/Float32.h>

/*namespace*/
using namespace std;
using namespace Eigen;

/*flag*/
bool init_pose_flag = false;
bool imu_flag = false;
bool odom_flag = false;
bool ndt_flag = false;

std::string map_frame_id = "map";
std::string odom_frame_id = "odom";

/*global variable*/
EKF ekf;
nav_msgs::Odometry ekf_odom;
MatrixXf x(3,1);        // 状態 (x,y,θ)
MatrixXf Sigma(3,3);
MatrixXf u(2,1);        // 制御 (v, w)
MatrixXf obs_ndt(3,1);  // NDT観測 (x,y,θ)

/*param*/
double init_x[3];       // 初期状態 (x,y,θ) [rad]
double init_sig[3];     // 初期分散 (sig_x, sig_y, sig_yaw)
double s_ndt[3];        // NDT観測値の分散 (sig_x, sig_y, sig_yaw)
double ndt_sig[2];
double s_input[4];      // 制御の誤差パラメータ (要素数[0],[2]は並進速度，[1],[3]は回頭速度のパラメータ)
float pitch;            // ピッチ角
std::string parent_frame_id;
bool mode_pointing_ini_pose_on_rviz;
bool ENABLE_TF, ENABLE_ODOM_TF;

tf::TransformBroadcaster* broadcaster_ptr = NULL;

void InputOdomCov(nav_msgs::Odometry& odom)
{
    /*x*/
    odom.pose.covariance[0] = Sigma(0, 0);  //x
    odom.pose.covariance[1] = Sigma(0, 1);  //y
    odom.pose.covariance[5] = Sigma(0, 2);  //yaw
    /*y*/
    odom.pose.covariance[6] = Sigma(1, 0);  //x
    odom.pose.covariance[7] = Sigma(1, 1);  //y
    odom.pose.covariance[11] = Sigma(1, 2); //yaw
    /*yaw*/
    odom.pose.covariance[30] = Sigma(2, 0); //x
    odom.pose.covariance[31] = Sigma(2, 1); //y
    odom.pose.covariance[35] = Sigma(2, 2); //yaw
}

MatrixXf predict(MatrixXf x, MatrixXf u, float dt, double *s_input, float pitch){
    /* u   : (v, w)の転置行列 v:並進速度, w:角速度
     * x   : (x, y, θ)の転置行列
     * dt      : 前ステップからの経過時間
     * s_input : 動作モデルのノイズパラメータ
     */
    MatrixXf Mu = MatrixXf::Zero(3,1); //今の位置からのpreのx
    MatrixXf P = MatrixXf::Zero(3,3);//sigma
    MatrixXf Gt= MatrixXf::Zero(3,3);//線形モデル 偏微分したもの
    MatrixXf Vt= MatrixXf::Zero(3,2);//ヤコビ
    MatrixXf Mt= MatrixXf::Zero(2,2);//分散共分散行列

    Mu = x;
    P = Sigma;

    Gt = ekf.jacobG(x, u, dt, pitch);
    Vt = ekf.jacobV(x, u, dt, pitch);
    Mt = ekf.jacobM(u, s_input);

    Mu = ekf.move(x, u, dt, pitch);
    P = Gt*Sigma*Gt.transpose() + Vt*Mt*Vt.transpose();
    Sigma = P;

    return Mu;
}

MatrixXf NDTUpdate(MatrixXf x){
    /* x    : 状態(x, y, yaw)の転置行列
     * u    : 制御(v, w)の転置行列
     * s_ndt: 観測ノイズ
     * sigma: 推定誤差
     */
    MatrixXf Mu= MatrixXf::Zero(3,1);//predictによる位置
    MatrixXf P = MatrixXf::Zero(3,3);//sigma
    MatrixXf Q= MatrixXf::Zero(3,3);//ndtのRt:共分散行列
    MatrixXf H= MatrixXf::Zero(3,3);//偏微分で求めるヤコビアン
    MatrixXf y= MatrixXf::Zero(3,1);//predictによる現在地とmeasurementによる推定値の差
    MatrixXf S= MatrixXf::Zero(3,3);//観測残差による共分散行列
    MatrixXf K= MatrixXf::Zero(3,3);//最適カルマンゲイン
    MatrixXf I = MatrixXf::Identity(3,3);//単位行列

    Mu = x;
    P = Sigma;

    Q.coeffRef(0,0) = (float)s_ndt[0];
    Q.coeffRef(1,1) = (float)s_ndt[1];
    Q.coeffRef(2,2) = (float)s_ndt[2];

    y = obs_ndt - ekf.h(x); //predictによる現在地とmeasurementによる推定値の差

    H = ekf.jacobH(x);

    S = H * Sigma * H.transpose() + Q;
    K = Sigma * H.transpose() * S.inverse();
    Mu = Mu + K*y;
    P = (I - K*H)*Sigma;
    Sigma = P;

    return Mu;
}



float expand(float after){

    static bool init_imu = true;
    static float before = 0.000001;
    static float sum;

    if(init_imu){
        before   = after;
        sum      = before;
        init_imu = false;
    }

    else{
        if((before * after) < 0){

            if(fabs(before) > M_PI/2){ //180度付近
                if(before > 0){
                    sum += (M_PI*2 - before + after);
                }
                else{
                    sum -= (M_PI*2 + before - after);
                }
            }
            else{
                sum += (before - after);
            }
        }

        else{
            sum += (after - before);
        }

        before = after;
    }

    return sum;
}



void odomCallback(nav_msgs::Odometry msg){
    u.coeffRef(0,0) = msg.twist.twist.linear.x;

    ekf_odom.header.stamp = msg.header.stamp; //
    ekf_odom.twist.twist.linear.x = u.coeffRef(0,0);

    /*input frame_id*/
    if(msg.child_frame_id != ""){
        ekf_odom.child_frame_id = msg.child_frame_id;
    }
    else{
        ekf_odom.child_frame_id = "/base_link";
        std::cout << "\033[33mchild_frame_id should be set. default '/base_link' is used\033[0m" << std::endl;
    }

    odom_frame_id = msg.header.frame_id;

    if(ENABLE_ODOM_TF){
        static Eigen::Vector3d first_odom_pose = Eigen::Vector3d::Zero();
        static double first_odom_yaw = 0;
        static bool first_odom_flag = true;
        Eigen::Vector3d odom_pose;
        double odom_yaw = tf::getYaw(msg.pose.pose.orientation);
        odom_pose << msg.pose.pose.position.x, msg.pose.pose.position.y, msg.pose.pose.position.z;
        if(first_odom_flag){
            first_odom_pose = odom_pose;
            first_odom_yaw = odom_yaw;
            std::cout << "first odom pose: \n" << first_odom_pose << std::endl;
            first_odom_flag = false;
        }
        odom_pose -= first_odom_pose;
        Eigen::AngleAxis<double> first_odom_yaw_rotation(-first_odom_yaw, Eigen::Vector3d::UnitZ());
        odom_pose = first_odom_yaw_rotation * odom_pose;
        odom_yaw -= first_odom_yaw;
        odom_yaw = atan2(sin(odom_yaw), cos(odom_yaw));
        tf::Transform transform;
        transform.setOrigin(tf::Vector3(odom_pose(0), odom_pose(1), odom_pose(2)));
        tf::Quaternion q;
        q.setRPY(0, 0, odom_yaw);
        transform.setRotation(q);
        tf::StampedTransform odom_tf(transform, msg.header.stamp, odom_frame_id, msg.child_frame_id);
        if(broadcaster_ptr != NULL){
            broadcaster_ptr->sendTransform(odom_tf);
        }
    }
    odom_flag = true;
}


void imuCallback(sensor_msgs::Imu::ConstPtr msg){
    u.coeffRef(1,0) = msg->angular_velocity.z;

    ekf_odom.twist.twist.angular.z = u.coeffRef(1,0);

    pitch = 0;
    imu_flag = true;
    // ekf_odom.header.stamp = msg->header.stamp; //

}


void ndtCallback(nav_msgs::Odometry msg){
    // ekf_odom.header.stamp = msg.header.stamp; //

    obs_ndt.coeffRef(0,0) = msg.pose.pose.position.x;
    obs_ndt.coeffRef(1,0) = msg.pose.pose.position.y;

    float yaw_true = expand(tf::getYaw(msg.pose.pose.orientation));
    obs_ndt.coeffRef(2,0) = yaw_true;

    map_frame_id = msg.header.frame_id;
    ndt_flag = true;
}

void hanteiCallback(const std_msgs::BoolConstPtr msg){

    if(msg->data){
        s_ndt[0] = 100;
        s_ndt[1] = 100;
        s_ndt[2] = 100;
    }


    else{
        //徐々に減らしている。
        s_ndt[0] = s_ndt[0] * 0.5;
        s_ndt[1] = s_ndt[1] * 0.5;
        s_ndt[2] = s_ndt[2] * 0.5;

        if(s_ndt[0] < 0.001){
            s_ndt[0] = 0.001;
            s_ndt[1] = 0.001;
            s_ndt[2] = 0.001;
        }
    }
    printf("NDT sig : %.4f\n", s_ndt[0]);
}


void initposeCallback(const geometry_msgs::PoseStampedConstPtr& msg){

    static bool init_flag_ = true;

    if(init_flag_ && mode_pointing_ini_pose_on_rviz){

        double qr,qp,qy;
        tf::Quaternion quat(msg->pose.orientation.x, msg->pose.orientation.y, msg->pose.orientation.z, msg->pose.orientation.w);
        tf::Matrix3x3(quat).getRPY(qr, qp, qy);


        init_x[0] = msg->pose.position.x;
        init_x[1] = msg->pose.position.y;
        init_x[2] = qy;

        x << init_x[0], init_x[1], init_x[2];

        obs_ndt.coeffRef(0,0) = init_x[0];
        obs_ndt.coeffRef(1,0) = init_x[1];
        obs_ndt.coeffRef(2,0) = init_x[2];

        init_flag_ = false;

    }

    init_pose_flag = true;
}




void poseInit(nav_msgs::Odometry &msg){
    msg.header.frame_id = parent_frame_id;
    /* msg.child_frame_id = "/matching_base_link"; */
    msg.pose.pose.position.x = init_x[0];
    msg.pose.pose.position.y = init_x[1];
    msg.pose.pose.position.z = 0.0;
    msg.pose.pose.orientation.x = 0.0;
    msg.pose.pose.orientation.y = 0.0;
    msg.pose.pose.orientation.z = init_x[2];
    msg.pose.pose.orientation.w = 0.0;

    x << init_x[0], init_x[1], init_x[2];
    Sigma << init_sig[0], 0, 0,
             0, init_sig[1], 0,
             0, 0, init_sig[2];
    u = MatrixXf::Zero(2,1);
    obs_ndt = MatrixXf::Zero(3,1);
}



void printParam(void){
    printf("Dgauss_ekf.cpp Parameters:\n");
    printf("Initial pose \n");
    printf("    init_x      : %lf\n", init_x[0]);
    printf("    init_y      : %lf\n", init_x[1]);
    printf("    init_yaw    : %lf\n", init_x[2]);
    printf("    init_sig_x  : %f\n", init_sig[0]);
    printf("    init_sig_y  : %f\n", init_sig[1]);
    printf("    init_sig_yaw    : %f\n", init_sig[2]);
    printf("Prediction \n");
    for(unsigned int i=0; i<sizeof(s_input)/sizeof(s_input[0]); i++){
        printf("    a%d     : %lf\n", i+1, s_input[i]);
    }
    printf("NDT Measurement \n");
    printf("    Sig_X       : %lf\n", s_ndt[0]);    ndt_sig[0] = s_ndt[0];
    printf("    Sig_Y       : %lf\n", s_ndt[1]);    ndt_sig[1] = s_ndt[1];
    printf("    Sig_Yaw     : %lf\n", s_ndt[2]);
    std::cout << "mode_pointing_ini_pose_on_rviz = " << (bool)mode_pointing_ini_pose_on_rviz << std::endl;
    std::cout << "ENABLE_TF = " << (bool)ENABLE_TF << std::endl;
    std::cout << "ENABLE_ODOM_TF = " << (bool)ENABLE_ODOM_TF << std::endl;
}



int main(int argc, char** argv){
    ros::init(argc, argv, "ekf");
    ros::NodeHandle n;
    ros::NodeHandle pnh("~");
    ROS_INFO("\033[1;32m---->\033[0m EKF Started.");

    //Subscribe
    ros::Subscriber odom_sub  = n.subscribe("/odom", 10, odomCallback);
    ros::Subscriber imu_sub   = n.subscribe("/imu/data", 10, imuCallback);
    ros::Subscriber ndt_sub   = n.subscribe("/NDT/result", 10, ndtCallback);//ndtによる結果
    ros::Subscriber hantei_sub = n.subscribe("/not_matching", 1, hanteiCallback);
    ros::Subscriber init_sub    = n.subscribe("/move_base_simple/goal", 1, initposeCallback);

    //Publish
    ros::Publisher ekf_pub = n.advertise<nav_msgs::Odometry>("/EKF/result", 100);
    ros::Publisher vis_ekf_pub = n.advertise<nav_msgs::Odometry>("/vis/odometry", 100);

    tf::TransformBroadcaster broadcaster;
    broadcaster_ptr = &broadcaster;
    tf::TransformListener listener;

    float dt;
    double last_time, now_time;

    //パラメータ
    pnh.param<double>("INIT_X", init_x[0], 0.0);
    pnh.param<double>("INIT_Y", init_x[1], 0.0);
    pnh.param<double>("INIT_YAW", init_x[2], -30.0);
    pnh.param<double>("init_sig_x", init_sig[0], 0.0);
    pnh.param<double>("init_sig_y", init_sig[1], 0.0);
    pnh.param<double>("init_sig_yaw", init_sig[2], 0.0);
    pnh.param<double>("Pred_a1", s_input[0], 0.0);
    pnh.param<double>("Pred_a2", s_input[1], 0.0);
    pnh.param<double>("Pred_a3", s_input[2], 0.0);
    pnh.param<double>("Pred_a4", s_input[3], 0.0);
    pnh.param<double>("NDT_sig_X", s_ndt[0], 0.0);
    pnh.param<double>("NDT_sig_Y", s_ndt[1], 0.0);
    pnh.param<double>("NDT_sig_Yaw", s_ndt[2], 0.0);
    pnh.param<std::string>("parent_frame_id", parent_frame_id, std::string("/map"));
    pnh.param<bool>("mode_pointing_ini_pose_on_rviz", mode_pointing_ini_pose_on_rviz, true);
    pnh.param<bool>("ENABLE_TF", ENABLE_TF, {false});
    pnh.param<bool>("ENABLE_ODOM_TF", ENABLE_ODOM_TF, {false});

    printParam();

    //初期化
    poseInit(ekf_odom);
    static bool init_flag = true;
    nav_msgs::Odometry vis_ekf;
    if(!mode_pointing_ini_pose_on_rviz){
        x << init_x[0], init_x[1], init_x[2];
        obs_ndt.coeffRef(0,0) = init_x[0];
        obs_ndt.coeffRef(1,0) = init_x[1];
        obs_ndt.coeffRef(2,0) = init_x[2];
        init_pose_flag = true;
    }

    last_time = ros::Time::now().toSec();

    const double HZ = 20.0;
    ros::Rate loop(HZ);
    while(ros::ok()){
        if(init_pose_flag){
            std::cout << "--- ndt odom ekf ---" << std::endl;
            if(imu_flag && odom_flag){
                if(init_flag){
                    now_time = ros::Time::now().toSec();
                    dt = 1.0 / HZ;
                    init_flag = false;
                }else{
                    now_time = ros::Time::now().toSec();
                    dt = now_time - last_time;
                }
                std::cout << "dt: " << dt << "[s]" << std::endl;
                std::cout << "before prediction: " << x.transpose() << std::endl;
                x = predict(x, u, dt, s_input, pitch);
                std::cout << "after prediction: " << x.transpose() << std::endl;

                if(ndt_flag){
                    x= NDTUpdate(x);
                }

                last_time = now_time;
                imu_flag = odom_flag = ndt_flag = false;
            }

            /*input odom covariance*/
            std::cout << "P: \n" << Sigma << std::endl;
            InputOdomCov(ekf_odom);

            ekf_odom.pose.pose.position.x = x.coeffRef(0,0);
            ekf_odom.pose.pose.position.y = x.coeffRef(1,0);
            ekf_odom.pose.pose.orientation = tf::createQuaternionMsgFromYaw(x.coeffRef(2, 0));
            ekf_pub.publish(ekf_odom);

            if(ENABLE_TF){
                try{
                    tf::Transform map_to_robot;
                    tf::poseMsgToTF(ekf_odom.pose.pose, map_to_robot);
                    tf::Stamped<tf::Pose> robot_to_map(map_to_robot.inverse(), ekf_odom.header.stamp, ekf_odom.child_frame_id);
                    tf::Stamped<tf::Pose> odom_to_map;
                    listener.transformPose(odom_frame_id, robot_to_map, odom_to_map);
                    broadcaster.sendTransform(tf::StampedTransform(odom_to_map.inverse(), ekf_odom.header.stamp, map_frame_id, odom_frame_id));
                    // broadcaster.sendTransform(tf::StampedTransform(map_to_robot, ekf_odom.header.stamp, map_frame_id, ekf_odom.child_frame_id));
                }catch(tf::TransformException ex){
                    std::cout << ex.what() << std::endl;
                }
            }

            vis_ekf = ekf_odom;
            vis_ekf_pub.publish(vis_ekf);
        }

        loop.sleep();
        ros::spinOnce();
    }

    return 0;
}
