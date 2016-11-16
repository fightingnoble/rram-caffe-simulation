#include "caffe/strategy.hpp"
#include "caffe/solver.hpp"
#include "caffe/sgd_solvers.hpp"

namespace caffe {
  template <typename Dtype>
  void ThresholdFailureStrategy<Dtype>::Apply() {
    const vector<Blob<Dtype>* >& net_weights = net_->failure_learnable_params();
    for (int i = 0; i < net_weights.size(); i++) {
      // threshold for this param
      Dtype rate = net_->params_lr()[i] * dynamic_cast<SGDSolver<Dtype>* >(const_cast<Solver<Dtype>* >(solver_))->GetLearningRate();
      Dtype threshold = threshold_ * rate;
      Dtype* diff_data = net_weights[i]->mutable_cpu_diff();
      int count = net_weights[i]->count();
      for (int j = 0; j < count; j ++) {
	// LOG(INFO) << "learning_rate: " << dynamic_cast<SGDSolver<Dtype>* >(const_cast<Solver<Dtype>* >(solver_))->GetLearningRate()
	// 	  << " param_lr: " << net_->params_lr()[i]
	// 	  << " diff: " << diff_data[j] 
	// 	  << " diff/(learning_rate*param_lr):" << diff_data[j]/rate;
 	if (fabs(diff_data[j]) <= threshold) {
	  //LOG(INFO) << "diff " << diff_data[j] << " less than threshold " << threshold_ << ". set to 0";
	  diff_data[j] = 0;
	  //LOG(INFO) << "will set " << diff_data[j] << " to 0.";
	}
      }
    }
  }

  // // 得到一个prune之后的flag blob
  // template <typename Dtype>
  // void GetPruneFlagMat(int id) {
  //   // prune_ratio
  //   Blob<Dtype>* weight_blob = net_->failure_learnable_params()[id];
  //   const Dtype* weights = weight_blob->cpu_data();
  //   weight_blob->count();
  //   std::sort(data, data + size, std::greater<Dtype>());
  //   net_->layer
  // }


  template <typename Dtype>
  void RemappingFailureStrategy<Dtype>::GetFailFlagMat(Blob<Dtype>* failure_blob, shared_ptr<Blob<Dtype> > flag_mat_p) {
    const Dtype* iters_p = failure_blob->cpu_data();
    const Dtype* values_p = failure_blob->cpu_diff();
    Dtype* flag_p = flag_mat_p->mutable_cpu_data();
    for (int i = 0; i < failure_blob->count(); i ++) {
      if (iters_p[i] < 0 && values_p[i] == 0) {
	flag_p[i] = 1; // when this cell fail and stuck at 0, set the flag to 1
      }
    }
  }

  template <typename Dtype>
  void RemappingFailureStrategy<Dtype>::SortFCNeurons(vector<vector<int> >& orders) {
    // neurons in the first layer are not sorted //  according to output weight failures
    int size = net_->fc_params_ids_.size();
    vector<shared_ptr<Blob<Dtype> > > flag_mat_vec;
    for (int i = 0; i < size; i++) {
      shared_ptr<Blob<Dtype> > flag_mat_p(new Blob<Dtype>()); // consist of 0 and 1, 1 if fail, 0 if not
      Blob<Dtype>* failure_blob = net_->failure_learnable_params()[net_->fc_params_ids_[i]];
      flag_mat_p->Reshape(failure_blob->shape());
      caffe_set<Dtype>(flag_mat_p->count(), Dtype(0), flag_mat_p->mutable_cpu_data()); // initialized 0 
      GetFailFlagMat(failure_blob, flag_mat_p);
      flag_mat_vec.push_back(flag_mat_p);
    }
    
    for (int i = 1; i < size; i++) {
      // calculate zeros of neurons in fc layer i
      vector<int> zero_nums;
      Blob<Dtype>* input_flag_blob = flag_mat_vec[i-1].get();
      Blob<Dtype>* output_flag_blob = flag_mat_vec[i].get();
      int last_layer_neurons = input_flag_blob->shape()[1];
      int next_layer_neurons = output_flag_blob->shape()[0];
      const Dtype* input_flag = input_flag_blob->cpu_data();
      const Dtype* output_flag = output_flag_blob->cpu_data();
      for (int j = 0; j < input_flag_blob->shape()[0]; j++) {
	// calculate input and output zero weights number of j-th neuron in fc layer i
	zero_nums.push_back(static_cast<int>(caffe_cpu_asum<Dtype>(last_layer_neurons, &input_flag[j * last_layer_neurons]) +
				   caffe_cpu_strided_asum<Dtype>(next_layer_neurons, &output_flag[j], input_flag_blob->shape()[0])));
      }
      // sort neuron index according to the zero nums
      // initialize original index locations
      std::vector<int> idx(zero_nums.size());
      std::iota(idx.begin(), idx.end(), 0);
      std::sort(idx.begin(), idx.end(),
	   [&zero_nums](size_t i1, size_t i2) {return zero_nums[i1] < zero_nums[i2];});
      orders.push_back(idx);
    }
  }

  template <typename Dtype>
  void RemappingFailureStrategy<Dtype>::Apply() {
    ++times_;
    if (times_ < start_ || (times_ - start_) % period_ != 0) {
      return;
    }
    int size = net_->fc_params_ids_.size();
    vector<vector<int> > orders;
    SortFCNeurons(orders); // orders size will be `size - 1`
    Blob<Dtype> remapped_weight;
    Blob<Dtype> remapped_bias;
    for (int i = 1; i < size; i++) {
      // remapping the neuron in th `i`-th fc layer
      vector<int>& order = orders[i-1];
      vector<int>& prune_order = prune_orders_[i-1];
      // rearrange the input weights
      Blob<Dtype>* input_weight_blob = net_->failure_learnable_params()[net_->fc_params_ids_[i-1]];
      int input_layer_dim = input_weight_blob->shape()[1];
      Blob<Dtype>* input_bias_blob = net_->failure_learnable_params()[net_->fc_params_ids_[i-1] + 1];
      remapped_weight.Reshape(input_weight_blob->shape());
      remapped_bias.Reshape(input_bias_blob->shape());
      caffe_copy(remapped_weight.count(), input_weight_blob->cpu_data(), remapped_weight.mutable_cpu_data());
      caffe_copy(remapped_weight.count(), input_weight_blob->cpu_diff(), remapped_weight.mutable_cpu_diff());
      caffe_copy(remapped_bias.count(), input_bias_blob->cpu_data(), remapped_bias.mutable_cpu_data());
      caffe_copy(remapped_bias.count(), input_bias_blob->cpu_diff(), remapped_bias.mutable_cpu_diff());
      for (int j = 0; j < order.size(); j++) {
	caffe_copy(input_layer_dim, remapped_weight.cpu_data() + prune_order[j] * input_layer_dim,
		   input_weight_blob->mutable_cpu_data() + order[j] * input_layer_dim);
	caffe_copy(input_layer_dim, remapped_weight.cpu_diff() + prune_order[j] * input_layer_dim,
		   input_weight_blob->mutable_cpu_diff() + order[j] * input_layer_dim);
	input_bias_blob->mutable_cpu_data()[order[j]] = remapped_weight.cpu_data()[prune_order[j]];
	input_bias_blob->mutable_cpu_diff()[order[j]] = remapped_weight.cpu_diff()[prune_order[j]];
      }
      // rearrange the output weights
      Blob<Dtype>* output_weight_blob = net_->failure_learnable_params()[net_->fc_params_ids_[i]];
      int output_layer_dim = output_weight_blob->shape()[1];
      int layer_dim = output_weight_blob->shape()[0];
      remapped_weight.Reshape(output_weight_blob->shape());
      caffe_copy(remapped_weight.count(), output_weight_blob->cpu_data(), remapped_weight.mutable_cpu_data());
      caffe_copy(remapped_weight.count(), output_weight_blob->cpu_diff(), remapped_weight.mutable_cpu_diff());
      for (int j = 0; j < order.size(); j++) {
	int from = prune_order[j];
	int to = order[j];
	for (int k = 0; k < output_layer_dim; k++) {
	  output_weight_blob->mutable_cpu_data()[k * layer_dim + to] = remapped_weight.cpu_data()[k * layer_dim + from];
	  output_weight_blob->mutable_cpu_diff()[k * layer_dim + to] = remapped_weight.cpu_diff()[k * layer_dim + from];
	}
      }
    }
  }

  INSTANTIATE_CLASS(FailureStrategy);
  INSTANTIATE_CLASS(ThresholdFailureStrategy);
  INSTANTIATE_CLASS(RemappingFailureStrategy);
} // namespace caffe
