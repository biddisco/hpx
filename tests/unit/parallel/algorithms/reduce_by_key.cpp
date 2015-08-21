//  Copyright (c) 2014-2015 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx_init.hpp>
#include <hpx/hpx.hpp>

#include "reduce_by_key_tests.hpp"

///////////////////////////////////////////////////////////////////////////////
template <typename IteratorTag>
void test_reduce_by_key1()
{
    using namespace hpx::parallel;

    test_reduce_by_key1(seq, IteratorTag());
    test_reduce_by_key1(par, IteratorTag());
    test_reduce_by_key1(par_vec, IteratorTag());

    test_reduce_by_key1_async(seq(task), IteratorTag());
    test_reduce_by_key1_async(par(task), IteratorTag());

    test_reduce_by_key1(execution_policy(seq), IteratorTag());
    test_reduce_by_key1(execution_policy(par), IteratorTag());
    test_reduce_by_key1(execution_policy(par_vec), IteratorTag());

    test_reduce_by_key1(execution_policy(seq(task)), IteratorTag());
    test_reduce_by_key1(execution_policy(par(task)), IteratorTag());
}

void reduce_by_key_test1()
{
    test_reduce_by_key1<std::random_access_iterator_tag>();
    test_reduce_by_key1<std::forward_iterator_tag>();
    test_reduce_by_key1<std::input_iterator_tag>();
}

///////////////////////////////////////////////////////////////////////////////
template <typename IteratorTag>
void test_reduce_by_key2()
{
    using namespace hpx::parallel;

    test_reduce_by_key2(seq, IteratorTag());
    test_reduce_by_key2(par, IteratorTag());
    test_reduce_by_key2(par_vec, IteratorTag());

    test_reduce_by_key2_async(seq(task), IteratorTag());
    test_reduce_by_key2_async(par(task), IteratorTag());

    test_reduce_by_key2(execution_policy(seq), IteratorTag());
    test_reduce_by_key2(execution_policy(par), IteratorTag());
    test_reduce_by_key2(execution_policy(par_vec), IteratorTag());

    test_reduce_by_key2(execution_policy(seq(task)), IteratorTag());
    test_reduce_by_key2(execution_policy(par(task)), IteratorTag());
}

void reduce_by_key_test2()
{
    test_reduce_by_key2<std::random_access_iterator_tag>();
    test_reduce_by_key2<std::forward_iterator_tag>();
    test_reduce_by_key2<std::input_iterator_tag>();
}

///////////////////////////////////////////////////////////////////////////////
template <typename IteratorTag>
void test_reduce_by_key3()
{
    using namespace hpx::parallel;

    test_reduce_by_key3(seq, IteratorTag());
    test_reduce_by_key3(par, IteratorTag());
    test_reduce_by_key3(par_vec, IteratorTag());

    test_reduce_by_key3_async(seq(task), IteratorTag());
    test_reduce_by_key3_async(par(task), IteratorTag());

    test_reduce_by_key3(execution_policy(seq), IteratorTag());
    test_reduce_by_key3(execution_policy(par), IteratorTag());
    test_reduce_by_key3(execution_policy(par_vec), IteratorTag());

    test_reduce_by_key3(execution_policy(seq(task)), IteratorTag());
    test_reduce_by_key3(execution_policy(par(task)), IteratorTag());
}

void reduce_by_key_test3()
{
    test_reduce_by_key3<std::random_access_iterator_tag>();
    test_reduce_by_key3<std::forward_iterator_tag>();
    test_reduce_by_key3<std::input_iterator_tag>();
}

///////////////////////////////////////////////////////////////////////////////
template <typename IteratorTag>
void test_reduce_by_key_exception()
{
    using namespace hpx::parallel;

    // If the execution policy object is of type vector_execution_policy,
    // std::terminate shall be called. therefore we do not test exceptions
    // with a vector execution policy
    test_reduce_by_key_exception(seq, IteratorTag());
    test_reduce_by_key_exception(par, IteratorTag());

    test_reduce_by_key_exception_async(seq(task), IteratorTag());
    test_reduce_by_key_exception_async(par(task), IteratorTag());

    test_reduce_by_key_exception(execution_policy(seq), IteratorTag());
    test_reduce_by_key_exception(execution_policy(par), IteratorTag());

    test_reduce_by_key_exception(execution_policy(seq(task)), IteratorTag());
    test_reduce_by_key_exception(execution_policy(par(task)), IteratorTag());
}

void reduce_by_key_exception_test()
{
    test_reduce_by_key_exception<std::random_access_iterator_tag>();
    test_reduce_by_key_exception<std::forward_iterator_tag>();
    test_reduce_by_key_exception<std::input_iterator_tag>();
}

///////////////////////////////////////////////////////////////////////////////
template <typename IteratorTag>
void test_reduce_by_key_bad_alloc()
{
    using namespace hpx::parallel;

    // If the execution policy object is of type vector_execution_policy,
    // std::terminate shall be called. therefore we do not test exceptions
    // with a vector execution policy
    test_reduce_by_key_bad_alloc(seq, IteratorTag());
    test_reduce_by_key_bad_alloc(par, IteratorTag());

    test_reduce_by_key_bad_alloc_async(seq(task), IteratorTag());
    test_reduce_by_key_bad_alloc_async(par(task), IteratorTag());

    test_reduce_by_key_bad_alloc(execution_policy(seq), IteratorTag());
    test_reduce_by_key_bad_alloc(execution_policy(par), IteratorTag());

    test_reduce_by_key_bad_alloc(execution_policy(seq(task)), IteratorTag());
    test_reduce_by_key_bad_alloc(execution_policy(par(task)), IteratorTag());
}

void reduce_by_key_bad_alloc_test()
{
    test_reduce_by_key_bad_alloc<std::random_access_iterator_tag>();
    test_reduce_by_key_bad_alloc<std::forward_iterator_tag>();
    test_reduce_by_key_bad_alloc<std::input_iterator_tag>();
}
////////////////////////////////////////////////////////////////////////////////
void reduce_by_key_validate()
{
    std::vector<int> a, b;
    // test scan algorithms using separate array for output
    //  std::cout << " Validating dual arrays " <<std::endl;
    test_reduce_by_key_validate(hpx::parallel::seq, a, b);
    test_reduce_by_key_validate(hpx::parallel::par, a, b);
    // test scan algorithms using same array for input and output
    //  std::cout << " Validating in_place arrays " <<std::endl;
    test_reduce_by_key_validate(hpx::parallel::seq, a, a);
    test_reduce_by_key_validate(hpx::parallel::par, a, a);
}

///////////////////////////////////////////////////////////////////////////////
int hpx_main(boost::program_options::variables_map& vm)
{
    unsigned int seed = (unsigned int)std::time(0);
    if (vm.count("seed"))
        seed = vm["seed"].as<unsigned int>();

    std::cout << "using seed: " << seed << std::endl;
    std::srand(seed);

    reduce_by_key_test1();
    reduce_by_key_test2();
    reduce_by_key_test3();

    reduce_by_key_exception_test();
    reduce_by_key_bad_alloc_test();

    reduce_by_key_validate();

    return hpx::finalize();
}

int main(int argc, char* argv[])
{
    // add command line option which controls the random number generator seed
    using namespace boost::program_options;
    options_description desc_commandline(
        "Usage: " HPX_APPLICATION_STRING " [options]");

    desc_commandline.add_options()
        ("seed,s", value<unsigned int>(),
        "the random number generator seed to use for this run")
        ;
    // By default this test should run on all available cores
    std::vector<std::string> cfg;
    cfg.push_back("hpx.os_threads=" +
        boost::lexical_cast<std::string>(hpx::threads::hardware_concurrency()));

    // Initialize and run HPX
    HPX_TEST_EQ_MSG(hpx::init(desc_commandline, argc, argv, cfg), 0,
        "HPX main exited with non-zero status");

    return hpx::util::report_errors();
}
