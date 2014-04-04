// gpp_python_model_selection.cpp
/*
  This file has the logic to invoke C++ functions pertaining to model selection from Python.
  The data flow follows the basic 4 step from gpp_python_common.hpp.

  Note: several internal functions of this source file are only called from Export*() functions,
  so their description, inputs, outputs, etc. comments have been moved. These comments exist in
  Export*() as Python docstrings, so we saw no need to repeat ourselves.
*/
// This include violates the Google Style Guide by placing an "other" system header ahead of C and C++ system headers.  However,
// it needs to be at the top, otherwise compilation fails on some systems with some versions of python: OS X, python 2.7.3.
// Putting this include first prevents pyport from doing something illegal in C++; reference: http://bugs.python.org/issue10910
#include "Python.h"  // NOLINT(build/include)

#include "gpp_python_model_selection.hpp"

// NOLINT-ing the C, C++ header includes as well; otherwise cpplint gets confused
#include <algorithm>  // NOLINT(build/include_order)
#include <limits>  // NOLINT(build/include_order)
#include <vector>  // NOLINT(build/include_order)

#include <boost/python/def.hpp>  // NOLINT(build/include_order)
#include <boost/python/dict.hpp>  // NOLINT(build/include_order)
#include <boost/python/extract.hpp>  // NOLINT(build/include_order)
#include <boost/python/list.hpp>  // NOLINT(build/include_order)
#include <boost/python/object.hpp>  // NOLINT(build/include_order)

#include "gpp_common.hpp"
#include "gpp_covariance.hpp"
#include "gpp_domain.hpp"
#include "gpp_exception.hpp"
#include "gpp_geometry.hpp"
#include "gpp_model_selection_and_hyperparameter_optimization.hpp"
#include "gpp_optimization_parameters.hpp"
#include "gpp_python_common.hpp"

namespace optimal_learning {

namespace {

double ComputeLogLikelihoodWrapper(const boost::python::list& points_sampled, const boost::python::list& points_sampled_value, int dim, int num_sampled, LogLikelihoodTypes objective_type, const boost::python::list& hyperparameters, const boost::python::list& noise_variance) {
  const int num_to_sample = 0;
  const boost::python::list points_to_sample_dummy;
  PythonInterfaceInputContainer input_container(hyperparameters, points_sampled, points_sampled_value, noise_variance, points_to_sample_dummy, dim, num_sampled, num_to_sample);

  SquareExponential square_exponential(input_container.dim, input_container.alpha, input_container.lengths.data());
  switch (objective_type) {
    case LogLikelihoodTypes::kLogMarginalLikelihood: {
      LogMarginalLikelihoodEvaluator log_marginal_eval(input_container.points_sampled.data(), input_container.points_sampled_value.data(), input_container.noise_variance.data(), input_container.dim, input_container.num_sampled);
      LogMarginalLikelihoodState log_marginal_state(log_marginal_eval, square_exponential);

      double log_likelihood = log_marginal_eval.ComputeLogLikelihood(log_marginal_state);
      return log_likelihood;
    }  // end case LogLikelihoodTypes::kLogMarginalLikelihood
    case LogLikelihoodTypes::kLeaveOneOutLogLikelihood: {
      LeaveOneOutLogLikelihoodEvaluator leave_one_out_eval(input_container.points_sampled.data(), input_container.points_sampled_value.data(), input_container.noise_variance.data(), input_container.dim, input_container.num_sampled);
      LeaveOneOutLogLikelihoodState leave_one_out_state(leave_one_out_eval, square_exponential);

      double loo_likelihood = leave_one_out_eval.ComputeLogLikelihood(leave_one_out_state);
      return loo_likelihood;
    }
    default: {
      double log_likelihood = -std::numeric_limits<double>::max();
      OL_THROW_EXCEPTION(RuntimeException, "ERROR: invalid objective mode choice. Setting log likelihood to -DBL_MAX.");
      return log_likelihood;
    }
  }  // end switch over objective_type
}

boost::python::list ComputeHyperparameterGradLogLikelihoodWrapper(const boost::python::list& points_sampled, const boost::python::list& points_sampled_value, int dim, int num_sampled, LogLikelihoodTypes objective_type, const boost::python::list& hyperparameters, const boost::python::list& noise_variance) {
  const int num_to_sample = 0;
  const boost::python::list points_to_sample_dummy;
  PythonInterfaceInputContainer input_container(hyperparameters, points_sampled, points_sampled_value, noise_variance, points_to_sample_dummy, dim, num_sampled, num_to_sample);

  SquareExponential square_exponential(input_container.dim, input_container.alpha, input_container.lengths.data());
  std::vector<double> grad_log_likelihood(square_exponential.GetNumberOfHyperparameters());
  switch (objective_type) {
    case LogLikelihoodTypes::kLogMarginalLikelihood: {
      LogMarginalLikelihoodEvaluator log_marginal_eval(input_container.points_sampled.data(), input_container.points_sampled_value.data(), input_container.noise_variance.data(), input_container.dim, input_container.num_sampled);
      LogMarginalLikelihoodState log_marginal_state(log_marginal_eval, square_exponential);

      log_marginal_eval.ComputeGradLogLikelihood(&log_marginal_state, grad_log_likelihood.data());
      break;
    }  // end case LogLikelihoodTypes::kLogMarginalLikelihood
    case LogLikelihoodTypes::kLeaveOneOutLogLikelihood: {
      LeaveOneOutLogLikelihoodEvaluator leave_one_out_eval(input_container.points_sampled.data(), input_container.points_sampled_value.data(), input_container.noise_variance.data(), input_container.dim, input_container.num_sampled);
      LeaveOneOutLogLikelihoodState leave_one_out_state(leave_one_out_eval, square_exponential);

      leave_one_out_eval.ComputeGradLogLikelihood(&leave_one_out_state, grad_log_likelihood.data());
      break;
    }
    default: {
      std::fill(grad_log_likelihood.begin(), grad_log_likelihood.end(), std::numeric_limits<double>::max());
      OL_THROW_EXCEPTION(RuntimeException, "ERROR: invalid objective mode choice. Setting all gradients to DBL_MAX.");
      break;
    }
  }  // end switch over objective_type

  return VectorToPylist(grad_log_likelihood);
}

/*
  Utility that dispatches log likelihood optimization (wrt hyperparameters) based on optimizer type.
  This is just used to reduce copy-pasted code.

  Let n_hyper = covariance.GetNumberOfHyperparameters();

  INPUTS:
  optimization_parameters: round_generation/MOE_driver.MOERunner.HyperparameterOptimizationParameters
      Python object containing the LogLikelihoodTypes objective_type and OptimizerTypes optimzer_typ
      to use as well as appropriate parameter structs e.g., NewtonParameters for type kNewton).
      See comments on the python interface for multistart_hyperparameter_optimization_wrapper
  log_likelihood_eval: object supporting evaluation of log likelihood
  covariance: the CovarianceFunction object encoding assumptions about the GP's behavior on our data
  hyperparameter_domain[2][n_hyper]: matrix specifying the boundaries of a n_hyper-dimensional tensor-product
                      domain.  Specified as a list of [x_i_min, x_i_max] pairs, i = 0 .. dim-1
                      Specify in LOG-10 SPACE!
  optimizer_type: type of optimization to use (e.g., null, gradient descent)
  max_num_threads: maximum number of threads for use by OpenMP (generally should be <= # cores)
  randomness_source: object containing randomness sources for generating random points in the domain
  status: pydict object; cannot be None

  OUTPUTS:
  randomness_source: PRNG internal states modified
  status: modified on exit to describe whether convergence occurred
  new_hyperparameters[n_hyper]: new hyperparameters found by optimizer to maximize the specified log likelihood measure
*/
template <typename LogLikelihoodEvaluator>
void DispatchHyperparameterOptimization(const boost::python::object& optimization_parameters, const LogLikelihoodEvaluator& log_likelihood_eval, const CovarianceInterface& covariance, ClosedInterval const * restrict hyperparameter_domain, OptimizerTypes optimizer_type, int max_num_threads, RandomnessSourceContainer& randomness_source, boost::python::dict& status, double * restrict new_hyperparameters) {
  bool found_flag = false;
  switch (optimizer_type) {
    case OptimizerTypes::kNull: {
      // found_flag set to true; 'dumb' search cannot fail
      // TODO(eliu): REMOVE this assumption and have 'dumb' search function pass
      // out found_flag like every other optimizer does!
      found_flag = true;

      // optimization_parameters must contain an int num_random_samples field, extract it
      int num_random_samples = boost::python::extract<int>(optimization_parameters.attr("num_random_samples"));
      LatinHypercubeSearchHyperparameterOptimization(log_likelihood_eval, covariance, hyperparameter_domain, num_random_samples, max_num_threads, &randomness_source.uniform_generator, new_hyperparameters);
      status["lhc_found_update"] = found_flag;
      break;
    }  // end case kNull for optimizer_type
    case OptimizerTypes::kGradientDescent: {
      // optimization_parameters must contain a optimizer_parameters field
      // of type GradientDescentParameters. extract it
      const GradientDescentParameters& gradient_descent_parameters = boost::python::extract<GradientDescentParameters&>(optimization_parameters.attr("optimizer_parameters"));
      MultistartGradientDescentHyperparameterOptimization(log_likelihood_eval, covariance, gradient_descent_parameters, hyperparameter_domain, max_num_threads, &found_flag, &randomness_source.uniform_generator, new_hyperparameters);
      status["gradient_descent_found_update"] = found_flag;
      break;
    }  // end case kGradientDescent for optimizer_type
    case OptimizerTypes::kNewton: {
      // optimization_parameters must contain a optimizer_parameters field
      // of type NewtonParameters. extract it
      const NewtonParameters& newton_parameters = boost::python::extract<NewtonParameters&>(optimization_parameters.attr("optimizer_parameters"));
      MultistartNewtonHyperparameterOptimization(log_likelihood_eval, covariance, newton_parameters, hyperparameter_domain, max_num_threads, &found_flag, &randomness_source.uniform_generator, new_hyperparameters);
      status["newton_found_update"] = found_flag;
      break;
    }  // end case kNewton for optimizer_type
    default: {
      std::fill(new_hyperparameters, new_hyperparameters + covariance.GetNumberOfHyperparameters(), 1.0);
      OL_THROW_EXCEPTION(RuntimeException, "ERROR: invalid optimizer choice. Setting all hyperparameters to 1.0.");
      break;
    }
  }  // end switch over optimzer_type for LogLikelihoodTypes::kLogMarginalLikelihood
}

boost::python::list MultistartHyperparameterOptimizationWrapper(const boost::python::object& optimization_parameters, const boost::python::list& hyperparameter_domain, const boost::python::list& points_sampled, const boost::python::list& points_sampled_value, int dim, int num_sampled, const boost::python::list& hyperparameters, const boost::python::list& noise_variance, int max_num_threads, RandomnessSourceContainer& randomness_source, boost::python::dict& status) {
  const int num_to_sample = 0;
  const boost::python::list points_to_sample_dummy;
  PythonInterfaceInputContainer input_container(hyperparameters, points_sampled, points_sampled_value, noise_variance, points_to_sample_dummy, dim, num_sampled, num_to_sample);

  SquareExponential square_exponential(input_container.dim, input_container.alpha, input_container.lengths.data());
  int num_hyperparameters = square_exponential.GetNumberOfHyperparameters();
  std::vector<double> new_hyperparameters(num_hyperparameters);

  std::vector<ClosedInterval> hyperparameter_domain_C(num_hyperparameters);
  CopyPylistToClosedIntervalVector(hyperparameter_domain, num_hyperparameters, hyperparameter_domain_C);

  OptimizerTypes optimizer_type = boost::python::extract<OptimizerTypes>(optimization_parameters.attr("optimizer_type"));
  LogLikelihoodTypes objective_type = boost::python::extract<LogLikelihoodTypes>(optimization_parameters.attr("objective_type"));
  switch (objective_type) {
    case LogLikelihoodTypes::kLogMarginalLikelihood: {
      LogMarginalLikelihoodEvaluator log_likelihood_eval(input_container.points_sampled.data(), input_container.points_sampled_value.data(), input_container.noise_variance.data(), input_container.dim, input_container.num_sampled);

      DispatchHyperparameterOptimization(optimization_parameters, log_likelihood_eval, square_exponential, hyperparameter_domain_C.data(), optimizer_type, max_num_threads, randomness_source, status, new_hyperparameters.data());
      break;
    }  // end case LogLikelihoodTypes::kLogMarginalLikelihood
    case LogLikelihoodTypes::kLeaveOneOutLogLikelihood: {
      LeaveOneOutLogLikelihoodEvaluator log_likelihood_eval(input_container.points_sampled.data(), input_container.points_sampled_value.data(), input_container.noise_variance.data(), input_container.dim, input_container.num_sampled);

      DispatchHyperparameterOptimization(optimization_parameters, log_likelihood_eval, square_exponential, hyperparameter_domain_C.data(), optimizer_type, max_num_threads, randomness_source, status, new_hyperparameters.data());
      break;
    }  // end case LogLikelihoodTypes::kLeaveOneOutLogLikelihood
    default: {
      std::fill(new_hyperparameters.begin(), new_hyperparameters.end(), 1.0);
      OL_THROW_EXCEPTION(RuntimeException, "ERROR: invalid objective type choice. Setting all hyperparameters to 1.0.");
      break;
    }
  }  // end switch over objective_type

  return VectorToPylist(new_hyperparameters);
}

boost::python::list EvaluateLogLikelihoodAtHyperparameterListWrapper(const boost::python::list& hyperparameter_list, const boost::python::list& points_sampled, const boost::python::list& points_sampled_value, int dim, int num_sampled, LogLikelihoodTypes objective_mode, const boost::python::list& hyperparameters, const boost::python::list& noise_variance, int num_multistarts, int max_num_threads) {
  const int num_to_sample = 0;
  const boost::python::list points_to_sample_dummy;
  PythonInterfaceInputContainer input_container(hyperparameters, points_sampled, points_sampled_value, noise_variance, points_to_sample_dummy, dim, num_sampled, num_to_sample);

  SquareExponential square_exponential(input_container.dim, input_container.alpha, input_container.lengths.data());

  std::vector<double> new_hyperparameters_C(square_exponential.GetNumberOfHyperparameters());
  std::vector<double> result_function_values_C(num_multistarts);
  std::vector<double> initial_guesses_C(square_exponential.GetNumberOfHyperparameters() * num_multistarts);

  CopyPylistToVector(hyperparameter_list, square_exponential.GetNumberOfHyperparameters() * num_multistarts, initial_guesses_C);

  TensorProductDomain dummy_domain(nullptr, 0);

  switch (objective_mode) {
    case LogLikelihoodTypes::kLogMarginalLikelihood: {
      LogMarginalLikelihoodEvaluator log_likelihood_eval(input_container.points_sampled.data(), input_container.points_sampled_value.data(), input_container.noise_variance.data(), input_container.dim, input_container.num_sampled);
      EvaluateLogLikelihoodAtPointList(log_likelihood_eval, square_exponential, dummy_domain, initial_guesses_C.data(), num_multistarts, max_num_threads, result_function_values_C.data(), new_hyperparameters_C.data());
      break;
    }
    case LogLikelihoodTypes::kLeaveOneOutLogLikelihood: {
      LeaveOneOutLogLikelihoodEvaluator log_likelihood_eval(input_container.points_sampled.data(), input_container.points_sampled_value.data(), input_container.noise_variance.data(), input_container.dim, input_container.num_sampled);
      EvaluateLogLikelihoodAtPointList(log_likelihood_eval, square_exponential, dummy_domain, initial_guesses_C.data(), num_multistarts, max_num_threads, result_function_values_C.data(), new_hyperparameters_C.data());
      break;
    }
    default: {
      std::fill(result_function_values_C.begin(), result_function_values_C.end(), -std::numeric_limits<double>::max());
      OL_THROW_EXCEPTION(RuntimeException, "ERROR: invalid objective mode choice. Setting all results to -DBL_MAX.");
      break;
    }
  }

  return VectorToPylist(result_function_values_C);
}

}  // end unnamed namespace

void ExportModelSelectionFunctions() {
  boost::python::def("compute_log_likelihood", ComputeLogLikelihoodWrapper, R"%%(
    Computes the specified log likelihood measure of model fit using the given
    hyperparameters.

    pylist points_sampled[num_sampled][dim]: points already sampled
    pylist points_sampled_value[num_sampled]: objective value at each sampled point
    int dim: dimension of parameter space
    int num_sampled: number of points already sampled
    LogLikelihoodTypes objective_mode: describes which log likelihood measure to compute (e.g., kLogMarginalLikelihood)
    pylist hyperparameters[2]: covariance hyperparameters; see "Details on ..." section at the top of BOOST_PYTHON_MODULE
    pylist noise_variance[num_sampled]: \sigma_n^2, noise variance (one value per sampled point)

    RETURNS:
    double result: computed log marginal likelihood of prior
    )%%");

  boost::python::def("compute_hyperparameter_grad_log_likelihood", ComputeHyperparameterGradLogLikelihoodWrapper, R"%%(
    Computes the gradient of the specified log likelihood measure of model fit using the given
    hyperparameters. Gradient computed wrt the given hyperparameters.

    n_hyper denotes the number of hyperparameters.

    pylist points_sampled[num_sampled][dim]: points already sampled
    pylist points_sampled_value[num_sampled]: objective value at each sampled point
    int dim: dimension of parameter space
    int num_sampled: number of points already sampled
    LogLikelihoodTypes objective_mode: describes which log likelihood measure to compute (e.g., kLogMarginalLikelihood)
    pylist hyperparameters[2]: covariance hyperparameters; see "Details on ..." section at the top of BOOST_PYTHON_MODULE
    pylist noise_variance[num_sampled]: \sigma_n^2, noise variance (one value per sampled point)

    RETURNS:
    pylist result[n_hyper]: gradients of log marginal likelihood wrt hyperparameters
    )%%");

  boost::python::def("multistart_hyperparameter_optimization", MultistartHyperparameterOptimizationWrapper, R"%%(
    Optimize the specified log likelihood measure over the specified domain using the specified optimization method.

    The HyperparameterOptimizationParameters object is a python class defined in:
    round_generation/MOE_driver.MOERunner.HyperparameterOptimizationParameters
    See that class definition for more details.

    This function expects it to have the fields:
    objective_type (LogLikelihoodTypes enum from this file)
    optimizer_type (OptimizerTypes enum from this file)
    num_random_samples (int, number of samples to 'dumb' search over, only used if optimizer_type == kNull)
    optimizer_parameters (*Parameters struct (gpp_optimization_parameters.hpp) where * matches optimizer_type
                          unused if optimizer_type == kNull)

    n_hyper denotes the number of hyperparameters.

    INPUTS:
    HyperparameterOptimizationParameters optimization_parameters:
        python object containing the LogLikelihoodTypes objective to use, OptimizerTypes optimzer_type
        to use as well as appropriate parameter structs e.g., NewtonParameters for type kNewton
    pylist hyperparameter_domain[2*n_hyper]: [lower, upper] bound pairs for each hyperparameter dimension in LOG-10 SPACE
    pylist points_sampled[num_sampled][dim]: points already sampled
    pylist points_sampled_value[num_sampled]: objective value at each sampled point
    int dim: dimension of parameter space
    int num_sampled: number of points already sampled
    pylist hyperparameters[2]: covariance hyperparameters; see "Details on ..." section at the top of BOOST_PYTHON_MODULE
    pylist noise_variance[num_sampled]: \sigma_n^2, noise variance (one value per sampled point)
    int max_num_threads: max number of threads to use during Newton optimization
    RandomnessSourceContainer randomness_source: object containing randomness source (UniformRandomGenerator) for LHC sampling
    pydict status: pydict object (cannot be None!); modified on exit to describe whether convergence occurred

    RETURNS::
    pylist next_hyperparameters[n_hyper]: optimized hyperparameters
    )%%");

  boost::python::def("evaluate_log_likelihood_at_hyperparameter_list", EvaluateLogLikelihoodAtHyperparameterListWrapper, R"%%(
    Evaluates the specified log likelihood measure of model fit at each member of
    hyperparameter_list. Useful for plotting.

    Equivalent to
    result = []
    for hyperparameters in hyperparameter_list:
        result.append(compute_log_likelihood(hyperparameters, ...))

    But this method is substantially faster (loops in C++ and is multithreaded).

    n_hyper denotes the number of hyperparameters

    INPUTS:
    pylist hyperparameter_list[num_multistarts][n_hyper]: list of hyperparameters at which to evaluate log likelihood
    pylist points_sampled[num_sampled][dim]: points already sampled
    pylist points_sampled_value[num_sampled]: objective value at each sampled point
    int dim: dimension of parameter space
    int num_sampled: number of points already sampled
    LogLikelihoodTypes objective_mode: describes which log likelihood measure to compute (e.g., kLogMarginalLikelihood)
    pylist hyperparameters[2]: covariance hyperparameters; see "Details on ..." section at the top of BOOST_PYTHON_MODULE
    pylist noise_variance[num_sampled]: \sigma_n^2, noise variance (one value per sampled point)
    num_multistarts: number of latin hypercube samples to use
    int max_num_threads: max number of threads to use during Newton optimization

    RETURNS:
    pylist result[num_multistarts]: log likelihood values at each point of the hyperparameter_list list, in the same order
    )%%");
}

}  // end namespace optimal_learning