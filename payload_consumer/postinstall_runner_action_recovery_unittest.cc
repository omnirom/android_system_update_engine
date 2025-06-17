//
// Copyright (C) 2012 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "gmock/gmock.h"
#include "update_engine/payload_consumer/postinstall_runner_action.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <utility>
#include "common/dynamic_partition_control_interface.h"

#include <base/bind.h>
#include <base/files/file_util.h>
#include <base/message_loop/message_loop.h>
#include <android-base/stringprintf.h>
#include <brillo/message_loops/base_message_loop.h>
#include <brillo/message_loops/message_loop_utils.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "update_engine/common/fake_boot_control.h"
#include "update_engine/common/fake_hardware.h"
#include "update_engine/common/test_utils.h"
#include "update_engine/common/utils.h"
#include "update_engine/common/mock_dynamic_partition_control.h"

using brillo::MessageLoop;
using chromeos_update_engine::test_utils::ScopedLoopbackDeviceBinder;
using std::string;
using testing::_;
using testing::AtLeast;
using testing::Return;

namespace chromeos_update_engine {

class PostinstActionProcessorDelegate : public ActionProcessorDelegate {
 public:
  PostinstActionProcessorDelegate() = default;
  void ProcessingDone(const ActionProcessor* processor,
                      ErrorCode code) override {
    MessageLoop::current()->BreakLoop();
    processing_done_called_ = true;
  }
  void ProcessingStopped(const ActionProcessor* processor) override {
    MessageLoop::current()->BreakLoop();
    processing_stopped_called_ = true;
  }

  void ActionCompleted(ActionProcessor* processor,
                       AbstractAction* action,
                       ErrorCode code) override {
    if (action->Type() == PostinstallRunnerAction::StaticType()) {
      code_ = code;
      code_set_ = true;
    }
  }

  ErrorCode code_{ErrorCode::kError};
  bool code_set_{false};
  bool processing_done_called_{false};
  bool processing_stopped_called_{false};
};

class MockPostinstallRunnerActionDelegate
    : public PostinstallRunnerAction::DelegateInterface {
 public:
  MOCK_METHOD1(ProgressUpdate, void(double progress));
};

class PostinstallRunnerActionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_.SetAsCurrent();
    {
      auto mock_dynamic_control =
          std::make_unique<MockDynamicPartitionControl>();
      mock_dynamic_control_ = mock_dynamic_control.get();
      fake_boot_control_.SetDynamicPartitionControl(
          std::move(mock_dynamic_control));
    }
    ON_CALL(*mock_dynamic_control_, FinishUpdate(_))
        .WillByDefault(Return(true));
    ON_CALL(*mock_dynamic_control_, GetVirtualAbFeatureFlag())
        .WillByDefault(Return(FeatureFlag(FeatureFlag::Value::LAUNCH)));
  }

  // Setup an action processor and run the PostinstallRunnerAction with a single
  // partition |device_path|, running the |postinstall_program| command from
  // there.
  void RunPostinstallAction(bool powerwash_required, bool save_rollback_data);

  void RunPostinstallActionWithInstallPlan(const InstallPlan& install_plan);

 public:
  void ResumeRunningAction() {
    ASSERT_NE(nullptr, postinstall_action_);
    postinstall_action_->ResumeAction();
  }

 protected:
  base::MessageLoopForIO base_loop_;
  brillo::BaseMessageLoop loop_{&base_loop_};

  FakeBootControl fake_boot_control_;
  FakeHardware fake_hardware_;
  MockDynamicPartitionControl* mock_dynamic_control_;
  PostinstActionProcessorDelegate processor_delegate_;

  // The PostinstallRunnerAction delegate receiving the progress updates.
  PostinstallRunnerAction::DelegateInterface* setup_action_delegate_{nullptr};

  // A pointer to the posinstall_runner action and the processor.
  PostinstallRunnerAction* postinstall_action_{nullptr};
  ActionProcessor* processor_{nullptr};
};

void PostinstallRunnerActionTest::RunPostinstallAction(
    bool powerwash_required, bool save_rollback_data) {
  InstallPlan::Partition part;
  part.name = "part";
  part.target_path = "/dev/invalid";
  part.readonly_target_path = "/dev/invalid";
  part.run_postinstall = false;
  part.postinstall_path.clear();
  InstallPlan install_plan;
  install_plan.partitions = {part};
  install_plan.download_url = "http://127.0.0.1:8080/update";
  install_plan.powerwash_required = powerwash_required;
  RunPostinstallActionWithInstallPlan(install_plan);
}

void PostinstallRunnerActionTest::RunPostinstallActionWithInstallPlan(
    const chromeos_update_engine::InstallPlan& install_plan) {
  ActionProcessor processor;
  processor_ = &processor;
  auto feeder_action = std::make_unique<ObjectFeederAction<InstallPlan>>();
  feeder_action->set_obj(install_plan);
  auto runner_action = std::make_unique<PostinstallRunnerAction>(
      &fake_boot_control_, &fake_hardware_);
  postinstall_action_ = runner_action.get();
  base::FilePath temp_dir;
  TEST_AND_RETURN(base::CreateNewTempDirectory("postinstall", &temp_dir));
  postinstall_action_->SetMountDir(temp_dir.value());
  runner_action->set_delegate(setup_action_delegate_);
  BondActions(feeder_action.get(), runner_action.get());
  auto collector_action =
      std::make_unique<ObjectCollectorAction<InstallPlan>>();
  BondActions(runner_action.get(), collector_action.get());
  processor.EnqueueAction(std::move(feeder_action));
  processor.EnqueueAction(std::move(runner_action));
  processor.EnqueueAction(std::move(collector_action));
  processor.set_delegate(&processor_delegate_);

  loop_.PostTask(
      FROM_HERE,
      base::Bind(
          [](ActionProcessor* processor) { processor->StartProcessing(); },
          base::Unretained(&processor)));
  loop_.Run();
  ASSERT_FALSE(processor.IsRunning());
  postinstall_action_ = nullptr;
  processor_ = nullptr;
  ASSERT_TRUE(processor_delegate_.processing_stopped_called_ ||
              processor_delegate_.processing_done_called_);
  if (processor_delegate_.processing_done_called_) {
    // Validation check that the code was set when the processor finishes.
    ASSERT_TRUE(processor_delegate_.code_set_);
  }
}

// Test that postinstall succeeds in the simple case of running the default
// /postinst command which only exits 0.
TEST_F(PostinstallRunnerActionTest, RunAsRootSimpleTest) {
  EXPECT_CALL(*mock_dynamic_control_, GetVirtualAbFeatureFlag())
      .WillOnce(Return(FeatureFlag(FeatureFlag::Value::LAUNCH)));
  RunPostinstallAction(false, false);
  ASSERT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
  ASSERT_TRUE(processor_delegate_.processing_done_called_);

  // Since powerwash_required was false, this should not trigger a powerwash.
  ASSERT_FALSE(fake_hardware_.IsPowerwashScheduled());
  ASSERT_FALSE(fake_hardware_.GetIsRollbackPowerwashScheduled());
}

}  // namespace chromeos_update_engine
