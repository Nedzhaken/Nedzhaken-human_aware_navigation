/**
 * BSD 3-Clause License
 *
 * Copyright (c) 2022, Zhi Yan
 * All rights reserved.
 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **/

// ROS
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <people_msgs/People.h>
#include <people_msgs/PositionMeasurementArray.h>
#include <visualization_msgs/MarkerArray.h>

// PCL
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/common/common.h>
#include <pcl/common/centroid.h>
#include <pcl/common/pca.h>

// CUDA-PCL
#include <cuda_runtime.h>
#include "cudaCluster.h"

// SVM
#include <libsvm/svm.h>

typedef struct feature {
  /*** for visualization ***/
  Eigen::Vector4f centroid;
  Eigen::Vector4f min;
  Eigen::Vector4f max;
  /*** for classification ***/
  int number_points;
  double min_distance;
  Eigen::Matrix3f covariance_3d;
  Eigen::Matrix3f moment_3d;
  // double partial_covariance_2d[9];
  // double histogram_main_2d[98];
  // double histogram_second_2d[45];
  double slice[20];
} Feature;

static const int FEATURE_SIZE = 34;

class Object3dDetector {
private:
  /*** ROS Publishers and Subscribers ***/
  ros::NodeHandle node_handle_;
  ros::Subscriber point_cloud_sub_;
  ros::Publisher people_pub_;
  ros::Publisher measurements_pub_;
  ros::Publisher marker_array_pub_;

  /*** ROS Parameters ***/
  bool print_fps_;
  std::string frame_id_;
  double z_limit_min_;
  double z_limit_max_;
  int cluster_size_min_;
  int cluster_size_max_;
  double human_probability_;
  bool human_size_limit_;
  std::string model_file_name_;
  std::string range_file_name_;
  
  /*** SVM stuffs ***/
  std::vector<Feature> features_;
  struct svm_node *svm_node_;
  struct svm_model *svm_model_;
  bool use_svm_model_;
  bool is_probability_model_;
  double svm_scale_range_[FEATURE_SIZE][2];
  double svm_x_lower_;
  double svm_x_upper_;
  
public:
  Object3dDetector();
  ~Object3dDetector();
  
  void pointCloudCallback(const sensor_msgs::PointCloud2::ConstPtr& ros_pc2);
  void extractCluster(pcl::PointCloud<pcl::PointXYZ>::Ptr pc);
  void extractFeature(pcl::PointCloud<pcl::PointXYZ>::Ptr pc, Feature &f, Eigen::Vector4f &min, Eigen::Vector4f &max, Eigen::Vector4f &centroid);
  void saveFeature(Feature &f, struct svm_node *x);
  void classify();
};

Object3dDetector::Object3dDetector() {
  ros::NodeHandle private_nh("~");
  
  people_pub_ = private_nh.advertise<people_msgs::People>("people", 100);
  measurements_pub_ = private_nh.advertise<people_msgs::PositionMeasurementArray>("measurements", 100);
  marker_array_pub_ = private_nh.advertise<visualization_msgs::MarkerArray>("markers", 100);
  
  point_cloud_sub_ = node_handle_.subscribe<sensor_msgs::PointCloud2>("rslidar_points", 1, &Object3dDetector::pointCloudCallback, this);
  
  private_nh.param<bool>("print_fps", print_fps_, false);
  private_nh.param<std::string>("frame_id", frame_id_, "rslidar");
  private_nh.param<double>("z_limit_min", z_limit_min_, -0.8);
  private_nh.param<double>("z_limit_max", z_limit_max_, 1.2);
  private_nh.param<int>("cluster_size_min", cluster_size_min_, 5);
  private_nh.param<int>("cluster_size_max", cluster_size_max_, 30000);
  private_nh.param<double>("human_probability", human_probability_, 0.7);
  private_nh.param<bool>("human_size_limit", human_size_limit_, false);
  /*** load a pre-trained svm model ***/
  private_nh.param<std::string>("model_file_name", model_file_name_, "");
  private_nh.param<std::string>("range_file_name", range_file_name_, "");
  
  use_svm_model_ = false;
  if((svm_model_ = svm_load_model(model_file_name_.c_str())) == NULL) {
    ROS_WARN("[object3d_detector_gpu] Can not load SVM model, use model-free detection.");
  } else {
    ROS_INFO("[object3d_detector_gpu] Load SVM model from '%s'.", model_file_name_.c_str());
    is_probability_model_ = svm_check_probability_model(svm_model_)?true:false;
    svm_node_ = (struct svm_node *)malloc((FEATURE_SIZE+1)*sizeof(struct svm_node)); // 1 more size for end index (-1)

    /*** load range file, c.f. https://github.com/cjlin1/libsvm/ ***/
    FILE *range_file = fopen(range_file_name_.c_str(),"r");
    if(range_file != NULL) {
      ROS_INFO("[object3d_detector_gpu] Load SVM range from '%s'.", range_file_name_.c_str());
      fscanf(range_file, "x\n");
      fscanf(range_file, "%lf %lf\n", &svm_x_lower_, &svm_x_upper_);
      //ROS_INFO_STREAM("svm_x_lower = " << svm_x_lower_ << ", svm_x_upper = " << svm_x_upper_);
      int idx = 1;
      double fmin, fmax;
      while(fscanf(range_file, "%d %lf %lf\n",&idx, &fmin, &fmax) == 3) {
	svm_scale_range_[idx-1][0] = fmin;
	svm_scale_range_[idx-1][1] =fmax;
	//ROS_INFO_STREAM(idx << " " << svm_scale_range_[idx-1][0] << " " << svm_scale_range_[idx-1][1]);
      }
      fclose(range_file);
      use_svm_model_ = true;
    } else {
      ROS_WARN("[object3d_detector_gpu] Can not load range file, use model-free detection.");
    }
  }
}

Object3dDetector::~Object3dDetector() {
  if(use_svm_model_) {
    svm_free_and_destroy_model(&svm_model_);
    free(svm_node_);
  }
}

int frames; clock_t start_time; bool reset = true;//fps
void Object3dDetector::pointCloudCallback(const sensor_msgs::PointCloud2::ConstPtr& ros_pc2) {
  if(print_fps_){if(reset){frames=0;start_time=clock();reset=false;}}//fps
  
  pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_pc(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::fromROSMsg(*ros_pc2, *pcl_pc);
  
  extractCluster(pcl_pc);
  classify();
  
  if(print_fps_){if(++frames>10){std::cerr<<"[object3d_detector_gpu]: fps = "<<double(frames)/(double(clock()-start_time)/CLOCKS_PER_SEC)<<", timestamp = "<<clock()/CLOCKS_PER_SEC<<std::endl;reset=true;}}//fps
}

const int nested_regions_ = 14;
int zone_[nested_regions_] = {2,3,3,3,3,3,3,2,3,3,3,3,3,3}; // for more details, see our IROS'17 paper.
void Object3dDetector::extractCluster(pcl::PointCloud<pcl::PointXYZ>::Ptr pc) {
  features_.clear();
  
  // Remove ground and ceiling
  std::vector<int> indices;
  for(int i = 0; i < pc->size(); ++i) {
    if(pc->points[i].z >= z_limit_min_ && pc->points[i].z <= z_limit_max_) {
      indices.push_back(i);
    }
  }
  pcl::copyPointCloud(*pc, indices, *pc);

  // Divide the point cloud into nested circular regions
  boost::array<std::vector<int>, nested_regions_> indices_array;
  for(int i = 0; i < pc->size(); i++) {
    double range = 0.0;
    for(int j = 0; j < nested_regions_; j++) {
      double d2 = pc->points[i].x * pc->points[i].x + pc->points[i].y * pc->points[i].y + pc->points[i].z * pc->points[i].z;
      if(d2 > range * range && d2 <= (range + zone_[j]) * (range + zone_[j])) {
      	indices_array[j].push_back(i);
      	break;
      }
      range += zone_[j];
    }
  }

  // Clustering
  double tolerance = 0.0;
  for(int i = 0; i < nested_regions_; i++) {
    tolerance += 0.1;
    if(indices_array[i].size() > cluster_size_min_) {
      pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_regionalized(new pcl::PointCloud<pcl::PointXYZ>);
      pcl::copyPointCloud(*pc, indices_array[i], *cloud_regionalized);
      
      cudaStream_t stream = NULL;
      cudaStreamCreate(&stream);
      
      float *inputEC = NULL;
      unsigned int sizeEC = cloud_regionalized->size();
      cudaMallocManaged(&inputEC, sizeof(float) * 4 * sizeEC, cudaMemAttachHost);
      cudaStreamAttachMemAsync(stream, inputEC);
      cudaMemcpyAsync(inputEC, cloud_regionalized->points.data(), sizeof(float) * 4 * sizeEC, cudaMemcpyHostToDevice, stream);
      cudaStreamSynchronize(stream);
      
      float *outputEC = NULL;
      cudaMallocManaged(&outputEC, sizeof(float) * 4 * sizeEC, cudaMemAttachHost);
      cudaStreamAttachMemAsync(stream, outputEC);
      cudaMemcpyAsync(outputEC, cloud_regionalized->points.data(), sizeof(float) * 4 * sizeEC, cudaMemcpyHostToDevice, stream);
      cudaStreamSynchronize(stream);
      
      unsigned int *indexEC = NULL;
      cudaMallocManaged(&indexEC, sizeof(float) * 4 * sizeEC, cudaMemAttachHost);
      cudaStreamAttachMemAsync(stream, indexEC);
      cudaMemsetAsync(indexEC, 0, sizeof(float) * 4 * sizeEC, stream);
      cudaStreamSynchronize(stream);
      
      extractClusterParam_t ecp;
      ecp.minClusterSize = cluster_size_min_;
      ecp.maxClusterSize = cluster_size_max_;
      ecp.voxelX = tolerance;
      ecp.voxelY = tolerance;
      ecp.voxelZ = tolerance;
      ecp.countThreshold = 0;
      cudaExtractCluster cudaec(stream);
      cudaec.set(ecp);
      
      cudaec.extract(inputEC, sizeEC, outputEC, indexEC);
      cudaStreamSynchronize(stream);
      
      for(int i = 1; i <= indexEC[0]; i++) {
	pcl::PointCloud<pcl::PointXYZ>::Ptr cluster(new pcl::PointCloud<pcl::PointXYZ>);
	cluster->width = indexEC[i];
	cluster->height = 1;
	cluster->points.resize(cluster->width * cluster->height);
	cluster->is_dense = true;
	
	unsigned int outoff = 0;
	for(int w = 1; w < i; w++) {
	  if(i > 1) {
	    outoff += indexEC[w];
	  }
	}
	
	for(std::size_t k = 0; k < indexEC[i]; ++k) {
	  cluster->points[k].x = outputEC[(outoff+k)*4+0];
	  cluster->points[k].y = outputEC[(outoff+k)*4+1];
	  cluster->points[k].z = outputEC[(outoff+k)*4+2];
	}
	
	Eigen::Vector4f min, max, centroid;
	pcl::getMinMax3D(*cluster, min, max);
      	pcl::compute3DCentroid(*cluster, centroid);
	
      	// Size limitation is not cool, but can increase fps
	if(human_size_limit_ &&
	   (max[0]-min[0] < 0.2 || max[0]-min[0] > 1.0 ||
	    max[1]-min[1] < 0.2 || max[1]-min[1] > 1.0 ||
	    max[2]-min[2] < 0.5 || max[2]-min[2] > 2.0)) {
	  continue;
	}
	
	Feature f;
	extractFeature(cluster, f, min, max, centroid);
	features_.push_back(f);
      }
      
      cudaFree(inputEC);
      cudaFree(outputEC);
      cudaFree(indexEC);
    }
  }
}

/* *** Feature Extraction ***
 * f1 (1d): the number of points included in a cluster.
 * f2 (1d): the minimum distance of the cluster to the sensor.
 * => f1 and f2 should be used in pairs, since f1 varies with f2 changes.
 * f3 (6d): 3D covariance matrix of the cluster.
 * f4 (6d): the normalized moment of inertia tensor.
 * => Since both f3 and f4 are symmetric, we only use 6 elements from each as features.
 * f5 (9d): 2D covariance matrix in 3 zones, which are the upper half, and the left and right lower halves.
 * f6 (98d): The normalized 2D histogram for the main plane, 14 × 7 bins.
 * f7 (45d): The normalized 2D histogram for the secondary plane, 9 × 5 bins.
 * f8 (20d): Slice feature for the cluster.
 */

void computeMomentOfInertiaTensorNormalized(pcl::PointCloud<pcl::PointXYZ> &pc, Eigen::Matrix3f &moment_3d) {
  moment_3d.setZero();
  for(size_t i = 0; i < pc.size(); i++) {
    moment_3d(0,0) += pc[i].y*pc[i].y+pc[i].z*pc[i].z;
    moment_3d(0,1) -= pc[i].x*pc[i].y;
    moment_3d(0,2) -= pc[i].x*pc[i].z;
    moment_3d(1,1) += pc[i].x*pc[i].x+pc[i].z*pc[i].z;
    moment_3d(1,2) -= pc[i].y*pc[i].z;
    moment_3d(2,2) += pc[i].x*pc[i].x+pc[i].y*pc[i].y;
  }
  moment_3d(1, 0) = moment_3d(0, 1);
  moment_3d(2, 0) = moment_3d(0, 2);
  moment_3d(2, 1) = moment_3d(1, 2);
}

/* Main plane is formed from the maximum and middle eigenvectors.
 * Secondary plane is formed from the middle and minimum eigenvectors.
 */
void computeProjectedPlane(pcl::PointCloud<pcl::PointXYZ>::Ptr pc, Eigen::Matrix3f &eigenvectors, int axe, Eigen::Vector4f &centroid, pcl::PointCloud<pcl::PointXYZ>::Ptr plane) {
  Eigen::Vector4f coefficients;
  coefficients[0] = eigenvectors(0,axe);
  coefficients[1] = eigenvectors(1,axe);
  coefficients[2] = eigenvectors(2,axe);
  coefficients[3] = 0;
  coefficients[3] = -1 * coefficients.dot(centroid);
  for(size_t i = 0; i < pc->size(); i++) {
    double distance_to_plane =
      coefficients[0] * pc->points[i].x +
      coefficients[1] * pc->points[i].y +
      coefficients[2] * pc->points[i].z +
      coefficients[3];
    pcl::PointXYZ p;
    p.x = pc->points[i].x - distance_to_plane * coefficients[0];
    p.y = pc->points[i].y - distance_to_plane * coefficients[1];
    p.z = pc->points[i].z - distance_to_plane * coefficients[2];
    plane->points.push_back(p);
  }
}

/* Upper half, and the left and right lower halves of a pedestrian. */
void compute3ZoneCovarianceMatrix(pcl::PointCloud<pcl::PointXYZ>::Ptr plane, Eigen::Vector4f &mean, double *partial_covariance_2d) {
  pcl::PointCloud<pcl::PointXYZ>::Ptr zone_decomposed[3];
  for(int i = 0; i < 3; i++)
    zone_decomposed[i].reset(new pcl::PointCloud<pcl::PointXYZ>);
  for(size_t i = 0; i < plane->size(); i++) {
    if(plane->points[i].z >= mean(2)) { // upper half
      zone_decomposed[0]->points.push_back(plane->points[i]);
    } else {
      if(plane->points[i].y >= mean(1)) // left lower half
	zone_decomposed[1]->points.push_back(plane->points[i]);
      else // right lower half
	zone_decomposed[2]->points.push_back(plane->points[i]);
    }
  }
  
  Eigen::Matrix3f covariance;
  Eigen::Vector4f centroid;
  for(int i = 0; i < 3; i++) {
    pcl::compute3DCentroid(*zone_decomposed[i], centroid);
    pcl::computeCovarianceMatrix(*zone_decomposed[i], centroid, covariance);
    partial_covariance_2d[i*3+0] = covariance(0,0);
    partial_covariance_2d[i*3+1] = covariance(0,1);
    partial_covariance_2d[i*3+2] = covariance(1,1);
  }
}

void computeHistogramNormalized(pcl::PointCloud<pcl::PointXYZ>::Ptr pc, int horiz_bins, int verti_bins, double *histogram) {
  Eigen::Vector4f min, max, min_box, max_box;
  pcl::getMinMax3D(*pc, min, max);
  double horiz_itv, verti_itv;
  horiz_itv = (max[0]-min[0]>max[1]-min[1]) ? (max[0]-min[0])/horiz_bins : (max[1]-min[1])/horiz_bins;
  verti_itv = (max[2] - min[2])/verti_bins;
  
  for(int i = 0; i < horiz_bins; i++) {
    for(int j = 0; j < verti_bins; j++) {
      if(max[0]-min[0] > max[1]-min[1]) {
	min_box << min[0]+horiz_itv*i, min[1], min[2]+verti_itv*j, 0;
	max_box << min[0]+horiz_itv*(i+1), max[1], min[2]+verti_itv*(j+1), 0;
      } else {
	min_box << min[0], min[1]+horiz_itv*i, min[2]+verti_itv*j, 0;
	max_box << max[0], min[1]+horiz_itv*(i+1), min[2]+verti_itv*(j+1), 0;
      }
      std::vector<int> indices;
      pcl::getPointsInBox(*pc, min_box, max_box, indices);
      histogram[i*verti_bins+j] = (double)indices.size() / (double)pc->size();
    }
  }
}

void computeSlice(pcl::PointCloud<pcl::PointXYZ>::Ptr pc, int n, double *slice) {
  for(int i = 0; i < 20; i++) {
    slice[i] = 0;
  }

  Eigen::Vector4f pc_min, pc_max;
  pcl::getMinMax3D(*pc, pc_min, pc_max);
  
  pcl::PointCloud<pcl::PointXYZ>::Ptr blocks[n];
  double itv = (pc_max[2] - pc_min[2]) / n;
  
  if(itv > 0) {
    for(int i = 0; i < n; i++) {
      blocks[i].reset(new pcl::PointCloud<pcl::PointXYZ>);
    }
    for(unsigned int i = 0, j; i < pc->size(); i++) {
      j = std::min((n-1), (int)((pc->points[i].z - pc_min[2]) / itv));
      blocks[j]->points.push_back(pc->points[i]);
    }
    
    Eigen::Vector4f block_min, block_max;
    for(int i = 0; i < n; i++) {
      if(blocks[i]->size() > 2) { // At least 3 points to perform pca.
	pcl::PCA<pcl::PointXYZ> pca;
	pcl::PointCloud<pcl::PointXYZ>::Ptr block_projected(new pcl::PointCloud<pcl::PointXYZ>);
	pca.setInputCloud(blocks[i]);
	pca.project(*blocks[i], *block_projected);
	pcl::getMinMax3D(*block_projected, block_min, block_max);
      } else {
	block_min.setZero();
	block_max.setZero();
      }
      slice[i*2] = block_max[0] - block_min[0];
      slice[i*2+1] = block_max[1] - block_min[1];
    }
  }
}

void Object3dDetector::extractFeature(pcl::PointCloud<pcl::PointXYZ>::Ptr pc, Feature &f, Eigen::Vector4f &min, Eigen::Vector4f &max, Eigen::Vector4f &centroid) {
  f.centroid = centroid;
  f.min = min;
  f.max = max;
  
  if(use_svm_model_) {
    // f1: Number of points included the cluster.
    f.number_points = pc->size();
    // f2: The minimum distance to the cluster.
    f.min_distance = FLT_MAX;
    double d2; //squared Euclidean distance
    for(int i = 0; i < pc->size(); i++) {
      d2 = pc->points[i].x*pc->points[i].x + pc->points[i].y*pc->points[i].y + pc->points[i].z*pc->points[i].z;
      if(f.min_distance > d2) {
	f.min_distance = d2;
      }
    }
    //f.min_distance = sqrt(f.min_distance);
    
    pcl::PCA<pcl::PointXYZ> pca;
    pcl::PointCloud<pcl::PointXYZ>::Ptr pc_projected(new pcl::PointCloud<pcl::PointXYZ>);
    pca.setInputCloud(pc);
    pca.project(*pc, *pc_projected);
    // f3: 3D covariance matrix of the cluster.
    pcl::computeCovarianceMatrixNormalized(*pc_projected, centroid, f.covariance_3d);
    // f4: The normalized moment of inertia tensor.
    computeMomentOfInertiaTensorNormalized(*pc_projected, f.moment_3d);
    // Navarro et al. assume that a pedestrian is in an upright position.
    //pcl::PointCloud<pcl::PointXYZ>::Ptr main_plane(new pcl::PointCloud<pcl::PointXYZ>), secondary_plane(new pcl::PointCloud<pcl::PointXYZ>);
    //computeProjectedPlane(pc, pca.getEigenVectors(), 2, centroid, main_plane);
    //computeProjectedPlane(pc, pca.getEigenVectors(), 1, centroid, secondary_plane);
    // f5: 2D covariance matrix in 3 zones, which are the upper half, and the left and right lower halves.
    //compute3ZoneCovarianceMatrix(main_plane, pca.getMean(), f.partial_covariance_2d);
    // f6 and f7
    //computeHistogramNormalized(main_plane, 7, 14, f.histogram_main_2d);
    //computeHistogramNormalized(secondary_plane, 5, 9, f.histogram_second_2d);
    // f8
    computeSlice(pc, 10, f.slice);
  }
}

void Object3dDetector::saveFeature(Feature &f, struct svm_node *x) {
  x[0].index  = 1;  x[0].value  = f.number_points; // libsvm indices start at 1
  x[1].index  = 2;  x[1].value  = f.min_distance;
  x[2].index  = 3;  x[2].value  = f.covariance_3d(0,0);
  x[3].index  = 4;  x[3].value  = f.covariance_3d(0,1);
  x[4].index  = 5;  x[4].value  = f.covariance_3d(0,2);
  x[5].index  = 6;  x[5].value  = f.covariance_3d(1,1);
  x[6].index  = 7;  x[6].value  = f.covariance_3d(1,2);
  x[7].index  = 8;  x[7].value  = f.covariance_3d(2,2);
  x[8].index  = 9;  x[8].value  = f.moment_3d(0,0);
  x[9].index  = 10; x[9].value  = f.moment_3d(0,1);
  x[10].index = 11; x[10].value = f.moment_3d(0,2);
  x[11].index = 12; x[11].value = f.moment_3d(1,1);
  x[12].index = 13; x[12].value = f.moment_3d(1,2);
  x[13].index = 14; x[13].value = f.moment_3d(2,2);
  // for(int i = 0; i < 9; i++) {
  //   x[i+14].index = i+15;
  //   x[i+14].value = f.partial_covariance_2d[i];
  // }
  // for(int i = 0; i < 98; i++) {
  // 	x[i+23].index = i+24;
  // 	x[i+23].value = f.histogram_main_2d[i];
  // }
  // for(int i = 0; i < 45; i++) {
  // 	x[i+121].index = i+122;
  // 	x[i+121].value = f.histogram_second_2d[i];
  // }
  for(int i = 0; i < 20; i++) {
    x[i+14].index = i+15;
    x[i+14].value = f.slice[i];
  }
  x[FEATURE_SIZE].index = -1;
  
  // for(int i = 0; i < FEATURE_SIZE; i++) {
  //   std::cerr << x[i].index << ":" << x[i].value << " ";
  //   std::cerr << std::endl;
  // }
}

void Object3dDetector::classify() {
  visualization_msgs::MarkerArray marker_array;
  people_msgs::PositionMeasurementArray pma;
  people_msgs::People ppl;
  
  for(std::vector<Feature>::iterator it = features_.begin(); it != features_.end(); ++it) {
    if(use_svm_model_) {
      saveFeature(*it, svm_node_);
      //std::cerr << "test_id = " << it->id << ", number_points = " << it->number_points << ", min_distance = " << it->min_distance << std::endl;
      
      // scale data
      for(int i = 0; i < FEATURE_SIZE; i++) {
	if(std::fabs(svm_scale_range_[i][0] - svm_scale_range_[i][1]) < DBL_EPSILON) { // skip single-valued attribute
	  continue;
	}
	if(std::fabs(svm_node_[i].value - svm_scale_range_[i][0]) < DBL_EPSILON) {
	  svm_node_[i].value = svm_x_lower_;
	}
	else if(std::fabs(svm_node_[i].value - svm_scale_range_[i][1]) < DBL_EPSILON) {
	  svm_node_[i].value = svm_x_upper_;
	}
	else {
	  svm_node_[i].value = svm_x_lower_ + (svm_x_upper_ - svm_x_lower_) * (svm_node_[i].value - svm_scale_range_[i][0]) / (svm_scale_range_[i][1] - svm_scale_range_[i][0]);
	}
      }
      
      // predict
      if(is_probability_model_) {
	double prob_estimates[svm_model_->nr_class];
	svm_predict_probability(svm_model_, svm_node_, prob_estimates);
	if(prob_estimates[0] < human_probability_) {
	  continue;
	}
      } else {
	if(svm_predict(svm_model_, svm_node_) != 1) {
	  continue;
	}
      }
    }
    
    visualization_msgs::Marker marker;
    marker.header.stamp = ros::Time::now();
    marker.header.frame_id = frame_id_;
    marker.ns = "object3d";
    marker.id = it-features_.begin();
    marker.type = visualization_msgs::Marker::LINE_LIST;

    geometry_msgs::Point p[24];
    p[0].x = it->max[0]; p[0].y = it->max[1]; p[0].z = it->max[2];
    p[1].x = it->min[0]; p[1].y = it->max[1]; p[1].z = it->max[2];
    p[2].x = it->max[0]; p[2].y = it->max[1]; p[2].z = it->max[2];
    p[3].x = it->max[0]; p[3].y = it->min[1]; p[3].z = it->max[2];
    p[4].x = it->max[0]; p[4].y = it->max[1]; p[4].z = it->max[2];
    p[5].x = it->max[0]; p[5].y = it->max[1]; p[5].z = it->min[2];
    p[6].x = it->min[0]; p[6].y = it->min[1]; p[6].z = it->min[2];
    p[7].x = it->max[0]; p[7].y = it->min[1]; p[7].z = it->min[2];
    p[8].x = it->min[0]; p[8].y = it->min[1]; p[8].z = it->min[2];
    p[9].x = it->min[0]; p[9].y = it->max[1]; p[9].z = it->min[2];
    p[10].x = it->min[0]; p[10].y = it->min[1]; p[10].z = it->min[2];
    p[11].x = it->min[0]; p[11].y = it->min[1]; p[11].z = it->max[2];
    p[12].x = it->min[0]; p[12].y = it->max[1]; p[12].z = it->max[2];
    p[13].x = it->min[0]; p[13].y = it->max[1]; p[13].z = it->min[2];
    p[14].x = it->min[0]; p[14].y = it->max[1]; p[14].z = it->max[2];
    p[15].x = it->min[0]; p[15].y = it->min[1]; p[15].z = it->max[2];
    p[16].x = it->max[0]; p[16].y = it->min[1]; p[16].z = it->max[2];
    p[17].x = it->max[0]; p[17].y = it->min[1]; p[17].z = it->min[2];
    p[18].x = it->max[0]; p[18].y = it->min[1]; p[18].z = it->max[2];
    p[19].x = it->min[0]; p[19].y = it->min[1]; p[19].z = it->max[2];
    p[20].x = it->max[0]; p[20].y = it->max[1]; p[20].z = it->min[2];
    p[21].x = it->min[0]; p[21].y = it->max[1]; p[21].z = it->min[2];
    p[22].x = it->max[0]; p[22].y = it->max[1]; p[22].z = it->min[2];
    p[23].x = it->max[0]; p[23].y = it->min[1]; p[23].z = it->min[2];

    for(int i = 0; i < 24; i++) {
      marker.points.push_back(p[i]);
    }
    marker.scale.x = 0.02;
    marker.color.a = 1.0;
    if(!use_svm_model_) {
      marker.color.r = 0.0;
      marker.color.g = 0.5;
      marker.color.b = 1.0;
    } else {
      marker.color.r = 0.0;
      marker.color.g = 1.0;
      marker.color.b = 0.5;
    }
    
    marker.lifetime = ros::Duration(0.1);
    marker_array.markers.push_back(marker);
    
    people_msgs::PositionMeasurement pm;
    pm.pos.x = it->centroid[0];
    pm.pos.y = it->centroid[1];
    pm.pos.z = it->centroid[2];
    pma.people.push_back(pm);

    people_msgs::Person ps;
    ps.position.x = it->centroid[0];
    ps.position.y = it->centroid[1];
    ps.position.z = it->centroid[2];
    ppl.people.push_back(ps);
  }
  
  if(marker_array.markers.size()) {
    marker_array_pub_.publish(marker_array);
  }
  
  if(pma.people.size()) {
    pma.header.stamp = ros::Time::now();
    pma.header.frame_id = frame_id_;
    measurements_pub_.publish(pma);
  }
  
  if(ppl.people.size()) {
    ppl.header.stamp = ros::Time::now();
    ppl.header.frame_id = frame_id_;
    people_pub_.publish(ppl);
  }
}

int main(int argc, char **argv) {
  ros::init(argc, argv, "object3d_detector_gpu");
  Object3dDetector d;
  ros::spin();
  return 0;
}
