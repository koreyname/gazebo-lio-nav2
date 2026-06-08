#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/point_cloud2.hpp"
#include "sensor_msgs/msg/point_field.hpp"
#include "sensor_msgs/point_cloud2_iterator.hpp"
#include "cstring"
#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <pcl_conversions/pcl_conversions.h>

struct LivoxPointXYZITL
{
    PCL_ADD_POINT4D;
    float intensity;
    uint8_t tag;
    uint8_t line;
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
} EIGEN_ALIGN16;
POINT_CLOUD_REGISTER_POINT_STRUCT(LivoxPointXYZITL,
                                  (float, x, x)(float, y, y)(float, z, z)(float, intensity, intensity)(uint8_t, tag, tag)(uint8_t, line, line))

struct OusterPointXYZIRT
{
    PCL_ADD_POINT4D;
    float intensity;
    uint32_t t;
    uint16_t reflectivity;
    uint8_t ring;
    uint16_t noise;
    uint32_t range;
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
} EIGEN_ALIGN16;
POINT_CLOUD_REGISTER_POINT_STRUCT(OusterPointXYZIRT,
                                  (float, x, x)(float, y, y)(float, z, z)(float, intensity, intensity)(uint32_t, t, t)(uint16_t, reflectivity, reflectivity)(uint8_t, ring, ring)(uint16_t, noise, noise)(uint32_t, range, range))

struct VelodynePointXYZIRT
{
    PCL_ADD_POINT4D
    PCL_ADD_INTENSITY;
    uint16_t ring;
    float time;
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
} EIGEN_ALIGN16;
POINT_CLOUD_REGISTER_POINT_STRUCT(VelodynePointXYZIRT,
                                  (float, x, x)(float, y, y)(float, z, z)(float, intensity, intensity)(uint16_t, ring, ring)(float, time, time))

pcl::PointCloud<VelodynePointXYZIRT>::Ptr cloud(new pcl::PointCloud<VelodynePointXYZIRT>);
class LaserPublisher : public rclcpp::Node
{
public:
    LaserPublisher() : Node("laser_publisher_node_cpp")
    {
        point_pub = this->create_publisher<sensor_msgs::msg::PointCloud2>("/pointcloud_out", 10);
        point_sub = this->create_subscription<sensor_msgs::msg::PointCloud2>(
            "/pointclouds", rclcpp::SensorDataQoS(),
            std::bind(&LaserPublisher::point_cb, this, std::placeholders::_1));
    }

private:
    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr point_pub;
    rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr point_sub;
    rclcpp::TimerBase::SharedPtr timer;
    double current_time = 0.0;
    void point_cb(const sensor_msgs::msg::PointCloud2::SharedPtr point)
    {
        pcl::PointCloud<pcl::PointXYZI>::Ptr input_cloud(new pcl::PointCloud<pcl::PointXYZI>);
        // ROS2PCL
        pcl::fromROSMsg(*point, *input_cloud);
        int num_lines = 16;
        pcl::PointCloud<VelodynePointXYZIRT>::Ptr output_cloud(new pcl::PointCloud<VelodynePointXYZIRT>);
        constexpr double min_vertical_angle = -15.0 * M_PI / 180.0;
        constexpr double vertical_fov = 30.0 * M_PI / 180.0;
        constexpr double scan_period = 1.0 / 8.0;

        output_cloud->points.reserve(input_cloud->points.size());
        for (const auto &pt : input_cloud->points)
        { // 生成输出点云
            VelodynePointXYZIRT pt_rt;
            pt_rt.x = pt.x;
            pt_rt.y = pt.y;
            pt_rt.z = pt.z;
            pt_rt.intensity = pt.intensity;
            // 计算这个点相对于雷达水平面的垂直角。
            double vertical_angle = atan2(pt.z, sqrt(pt.x * pt.x + pt.y * pt.y));
            /*
             * -15° -> ring 0
                0° -> ring 7/8
                +15° -> ring 15
            */
            int ring = static_cast<int>(
                std::round((vertical_angle - min_vertical_angle) /
                           vertical_fov * (num_lines - 1)));
            pt_rt.ring = static_cast<uint16_t>(
                std::max(0, std::min(num_lines - 1, ring)));

            double horizontal_angle = atan2(pt.y, pt.x);

            if (horizontal_angle < 0.0)
                horizontal_angle += 2.0 * M_PI;
            // 计算水平角，并转到：0 ~ 2π，估算这个点在一帧扫描里的相对时间
            pt_rt.time = static_cast<float>(
                horizontal_angle / (2.0 * M_PI) * scan_period);
            output_cloud->points.push_back(pt_rt);
        }

        output_cloud->width = output_cloud->points.size();
        output_cloud->height = 1;
        output_cloud->is_dense = input_cloud->is_dense;
        sensor_msgs::msg::PointCloud2 output_msg;
        // PCLtoROS
        pcl::toROSMsg(*output_cloud, output_msg);
        output_msg.header.frame_id = "lidar_link";
        output_msg.header.stamp = point->header.stamp;
        point_pub->publish(output_msg);
    }
};

int main(int argc, char const *argv[])
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<LaserPublisher>());
    rclcpp::shutdown();
    return 0;
}
