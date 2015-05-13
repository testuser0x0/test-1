#include "isaac/backend/templates/vaxpy.h"
#include "isaac/backend/keywords.h"
#include "isaac/driver/backend.h"
#include "isaac/tools/make_map.hpp"
#include "isaac/tools/make_vector.hpp"
#include "isaac/tools/to_string.hpp"
#include <iostream>

namespace isaac
{


vaxpy_parameters::vaxpy_parameters(unsigned int _simd_width,
                       unsigned int _group_size, unsigned int _num_groups,
                       fetching_policy_type _fetching_policy) :
      base::parameters_type(_simd_width, _group_size, 1, 1), num_groups(_num_groups), fetching_policy(_fetching_policy)
{ }


int vaxpy::is_invalid_impl(driver::Device const &, expressions_tuple const &) const
{
  if (p_.fetching_policy==FETCH_FROM_LOCAL)
    return TEMPLATE_INVALID_FETCHING_POLICY_TYPE;
  return TEMPLATE_VALID;
}

std::string vaxpy::generate_impl(const char * suffix, expressions_tuple const & expressions, driver::Device const & device, std::vector<mapping_type> const & mappings) const
{
  driver::backend_type backend = device.backend();
  std::string _size_t = size_type(device);

  kernel_generation_stream stream;
  std::string str_simd_width = tools::to_string(p_.simd_width);
  std::string dtype = append_width("#scalartype",p_.simd_width);

  switch(backend)
  {
#ifdef ISAAC_WITH_CUDA
    case driver::CUDA: stream << "#include  \"helper_math.h\"" << std::endl; break;
#endif
    case driver::OPENCL: stream << " __attribute__((reqd_work_group_size(" << p_.local_size_0 << "," << p_.local_size_1 << ",1)))" << std::endl; break;
  }

  stream << KernelPrefix(backend) << " void " << "axpy" << suffix << "(" << _size_t << " N," << generate_arguments(dtype, device, mappings, expressions) << ")" << std::endl;
  stream << "{" << std::endl;
  stream.inc_tab();

  process(stream, PARENT_NODE_TYPE, {{"array0", "#scalartype #namereg = #pointer[#start];"},
                                      {"array1", "#pointer += #start;"}}, expressions, mappings);

  stream << _size_t << " idx = " << GlobalIdx0(backend) << ";" << std::endl;
  stream << _size_t << " gsize = " << GlobalSize0(backend) << ";" << std::endl;

  std::string init, upper_bound, inc;
  fetching_loop_info(p_.fetching_policy, "N/"+str_simd_width, stream, init, upper_bound, inc, "idx", "gsize", device);

  stream << "for(" << _size_t << " i = " << init << "; i < " << upper_bound << "; i += " << inc << ")" << std::endl;
  stream << "{" << std::endl;
  stream.inc_tab();
  process(stream, PARENT_NODE_TYPE, {{"array1", dtype + " #namereg = #pointer[i*#stride];"},
                                     {"matrix_row", "#scalartype #namereg = $VALUE{#row*#stride1, i*#stride2};"},
                                     {"matrix_column", "#scalartype #namereg = $VALUE{i*#stride1,#column*#stride2};"},
                                     {"matrix_diag", "#scalartype #namereg = #pointer[#diag_offset<0?$OFFSET{(i - #diag_offset)*#stride1, i*#stride2}:$OFFSET{i*#stride1, (i + #diag_offset)*#stride2}];"}}, expressions, mappings);


  evaluate(stream, PARENT_NODE_TYPE, {{"array0", "#namereg"}, {"array1", "#namereg"},
                                      {"matrix_row", "#namereg"}, {"matrix_column", "#namereg"}, {"matrix_diag", "#namereg"},
                                      {"cast", CastPrefix(backend, dtype).get()}, {"host_scalar", p_.simd_width==1?"#name": InitPrefix(backend, dtype).get() + "(#name)"}}, expressions, mappings);



  process(stream, LHS_NODE_TYPE, {{"array1", "#pointer[i*#stride] = #namereg;"},
                                  {"matrix_row", "$VALUE{#row, i} = #namereg;"},
                                  {"matrix_column", "$VALUE{i, #column} = #namereg;"},
                                  {"matrix_diag", "#diag_offset<0?$VALUE{(i - #diag_offset)*#stride1, i*#stride2}:$VALUE{i*#stride1, (i + #diag_offset)*#stride2} = #namereg;"}}, expressions, mappings);

  stream.dec_tab();
  stream << "}" << std::endl;

  stream << "if(idx==0)" << std::endl;
  stream << "{" << std::endl;
  stream.inc_tab();
  process(stream, LHS_NODE_TYPE, tools::make_map<std::map<std::string, std::string> >("array0", "#pointer[#start] = #namereg;"), expressions, mappings);
  stream.dec_tab();
  stream << "}" << std::endl;

  stream.dec_tab();
  stream << "}" << std::endl;

  return stream.str();
}

vaxpy::vaxpy(vaxpy_parameters const & parameters,
                               binding_policy_t binding_policy) :
    base_impl<vaxpy, vaxpy_parameters>(parameters, binding_policy)
{}

vaxpy::vaxpy(unsigned int simd, unsigned int ls, unsigned int ng,
                               fetching_policy_type fetch, binding_policy_t bind):
    base_impl<vaxpy, vaxpy_parameters>(vaxpy_parameters(simd,ls,ng,fetch), bind)
{}


std::vector<int_t> vaxpy::input_sizes(expressions_tuple const & expressions)
{
  int_t size = static_cast<array_expression const *>(expressions.data().front().get())->shape()[0];
  return tools::make_vector<int_t>() << size;
}

void vaxpy::enqueue(driver::CommandQueue & queue, driver::Program & program, const char * suffix, base & fallback, controller<expressions_tuple> const & controller)
{
  expressions_tuple const & expressions = controller.x();
  //Size
  int_t size = input_sizes(expressions)[0];
  //Fallback
  if(p_.simd_width > 1 && (requires_fallback(expressions) || (size%p_.simd_width>0)))
  {
      fallback.enqueue(queue, program, "fallback", fallback, controller);
      return;
  }
  //Kernel
  char name[32] = {"axpy"};
  strcat(name, suffix);
  driver::Kernel kernel(program, name);
  //NDRange
  driver::NDRange global(p_.local_size_0*p_.num_groups);
  driver::NDRange local(p_.local_size_0);
  //Arguments
  unsigned int current_arg = 0;
  kernel.setSizeArg(current_arg++, size);
  set_arguments(expressions, kernel, current_arg);
  controller.execution_options().enqueue_cache(queue, kernel, global, local);
}


}
