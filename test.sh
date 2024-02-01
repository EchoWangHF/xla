set -euox pipefail -o history

#export XLA_DIR=$(git rev-parse --show-toplevel)
##CACHE_PATH="/data/tensorflow/archives/openxla"
##CACHE_PATH="/mnt/e/workspace/openxla"
#RC_FILE="${XLA_DIR}/.bazelrc"
#TARGET_FILTER=""
#TAGS_FILTER="-no_oss,-oss_excluded,-oss_serial"
#ADDITIONAL_FLAGS=""
#
##RC_FILE="/usertools/cpu.bazelrc"
##TAGS_FILTER="$TAGS_FILTER,-gpu,-mlu,-requires-gpu-nvidia,-requires-mlu"
#TAGS_FILTER="$TAGS_FILTER,-gpu,-requires-gpu-nvidia"
#ADDITIONAL_FLAGS="$ADDITIONAL_FLAGS --config=nonccl --local_test_jobs=4"
#
## Build & test XLA
##bazel --bazelrc=$RC_FILE \
#bazel build --config=gpu --copt=-w -k \
#      --verbose_failures \
#      --build_tag_filters=$TAGS_FILTER  \
#      --test_tag_filters=$TAGS_FILTER \
#      --keep_going \
#      --features=layering_check \
#      --flaky_test_attempts=3 \
#      --jobs=4 \
#      --nobuild_tests_only \
#      --distdir=${CACHE_PATH} \
#      --spawn_strategy=sandboxed \
#      $ADDITIONAL_FLAGS \
#      -- //xla/service/gpu/tests:element_wise_row_vectorization_test \
#      #-- //xla/... //build_tools/... $TARGET_FILTER
#
#

 #CACHE_PATH="/data/tensorflow/archives/openxla"
 #
 #rm -rf $TF_DUMP_GRAPH_PREFIX
 #
 ##bazel test --config=cuda --copt=-w \
 #bazel test --config=cuda --compilation_mode=dbg --copt=-g --copt=-w \
 #  --build_tag_filters=gpu,requires-gpu-nvidia,-no_gpu \
 #  --test_tag_filters=gpu,requires-gpu-nvidia,-no_gpu \
 #  --test_env="TF_CPP_MAX_VLOG_LEVEL=0" \
 #  --test_summary=detailed \
 #  --cache_test_results=no \
 #  --flaky_test_attempts=1 \
 #  --runs_per_test=1 \
 #  --nobuild_tests_only \
 #  --jobs=32 \
 #  --test_output=all \
 #  --test_verbose_timeout_warnings \
 #  --test_env=XLA_FLAGS="--xla_dump_hlo_as_text --xla_dump_to=${TF_DUMP_GRAPH_PREFIX}" \
 #  //xla/service/gpu:ir_emitter_triton_test

export TEST_RES_DIR="${PWD}/tmp"
rm -rf $TEST_RES_DIR
      #--test_env=XLA_FLAGS="--xla_dump_hlo_as_text --xla_dump_to=${TEST_RES_DIR}" \
bazel test --config=cuda --copt=-g --copt=-w \
      --test_output=all \
      --spawn_strategy=sandboxed \
      --keep_going \
      --jobs=4 \
      --test_env="TF_CPP_MAX_VLOG_LEVEL=0" \
      --verbose_failures \
      --test_summary=detailed \
      --cache_test_results=no \
      --flaky_test_attempts=1 \
      --test_verbose_timeout_warnings \
      --runs_per_test=1 \
      --nobuild_tests_only \
      //xla/service/gpu/tests:element_wise_row_vectorization_test \



