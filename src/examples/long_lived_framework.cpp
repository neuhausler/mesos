// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <glog/logging.h>

#include <iostream>
#include <string>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

using namespace mesos;

using std::string;
using std::vector;


// NOTE: Per-task resources are nominal because all of the resources for the
// container are provisioned when the executor is created. The executor can
// run multiple tasks at once, but uses a constant amount of resources
// regardless of the number of tasks.
const double CPUS_PER_TASK = 0.001;
const int32_t MEM_PER_TASK = 1;

const double CPUS_PER_EXECUTOR = 0.1;
const int32_t MEM_PER_EXECUTOR = 32;


// This scheduler picks one slave and repeatedly launches sleep tasks on it,
// using a single multi-task executor. If the slave or executor fails, the
// scheduler will pick another slave and continue launching sleep tasks.
class LongLivedScheduler : public Scheduler
{
public:
  explicit LongLivedScheduler(const ExecutorInfo& _executor)
    : executor(_executor),
      taskResources(Resources::parse(
          "cpus:" + stringify(CPUS_PER_TASK) +
          ";mem:" + stringify(MEM_PER_TASK)).get()),
      tasksLaunched(0) {}

  virtual ~LongLivedScheduler() {}

  virtual void registered(SchedulerDriver*,
                          const FrameworkID&,
                          const MasterInfo&)
  {
    LOG(INFO) << "Registered!";
  }

  virtual void reregistered(SchedulerDriver*, const MasterInfo& masterInfo)
  {
    LOG(INFO) << "Re-registered!";
  }

  virtual void disconnected(SchedulerDriver* driver)
  {
    LOG(INFO) << "Disconnected!";
  }

  virtual void resourceOffers(SchedulerDriver* driver,
                              const vector<Offer>& offers)
  {
    static const Resources EXECUTOR_RESOURCES = Resources(executor.resources());

    foreach (const Offer& offer, offers) {
      if (slaveId.isNone()) {
        // No active executor running in the cluster.
        // Launch a new task with executor.

        if (Resources(offer.resources()).flatten()
            .contains(EXECUTOR_RESOURCES + taskResources)) {
          LOG(INFO)
            << "Starting executor and task " << tasksLaunched
            << " on " << offer.hostname();

          launchTask(driver, offer);

          slaveId = offer.slave_id();
        } else {
          declineOffer(driver, offer);
        }
      } else if (slaveId == offer.slave_id()) {
        // Offer from the same slave that has an active executor.
        // Launch more tasks on that executor.

        if (Resources(offer.resources()).flatten().contains(taskResources)) {
          LOG(INFO)
            << "Starting task " << tasksLaunched << " on " << offer.hostname();

          launchTask(driver, offer);
        } else {
          declineOffer(driver, offer);
        }
      } else {
        // We have an active executor but this offer comes from a
        // different slave; decline the offer.
        declineOffer(driver, offer);
      }
    }
  }

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId) {}

  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    LOG(INFO)
      << "Task " << status.task_id().value()
      << " is in state " << TaskState_Name(status.state())
      << (status.has_message() ? " with message: " + status.message() : "");
  }

  virtual void frameworkMessage(SchedulerDriver* driver,
                                const ExecutorID& executorId,
                                const SlaveID& slaveId,
                                const string& data) {}

  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& _slaveId)
  {
    LOG(INFO) << "Slave lost: " << _slaveId;

    if (slaveId == _slaveId) {
      slaveId = None();
    }
  }

  virtual void executorLost(SchedulerDriver* driver,
                            const ExecutorID& executorId,
                            const SlaveID& _slaveId,
                            int status)
  {
    LOG(INFO)
      << "Executor '" << executorId << "' lost on slave "
      << _slaveId << " with status: " << status;

    slaveId = None();
  }

  virtual void error(SchedulerDriver* driver, const string& message) {}

private:
  // Helper to decline an offer.
  void declineOffer(SchedulerDriver* driver, const Offer& offer)
  {
    Filters filters;
    filters.set_refuse_seconds(600);

    driver->declineOffer(offer.id(), filters);
  }

  // Helper to launch a task using an offer.
  void launchTask(SchedulerDriver* driver, const Offer& offer)
  {
    int taskId = tasksLaunched++;

    TaskInfo task;
    task.set_name("Task " + stringify(taskId));
    task.mutable_task_id()->set_value(stringify(taskId));
    task.mutable_slave_id()->MergeFrom(offer.slave_id());
    task.mutable_resources()->CopyFrom(taskResources);
    task.mutable_executor()->CopyFrom(executor);

    driver->launchTasks(offer.id(), {task});
  }

  const ExecutorInfo executor;
  const Resources taskResources;
  string uri;
  int tasksLaunched;

  // The slave that is running the long-lived-executor.
  // Unless that slave/executor dies, this framework will not launch
  // an executor on any other slave.
  Option<SlaveID> slaveId;
};


class Flags : public flags::FlagsBase
{
public:
  Flags()
  {
    add(&master,
        "master",
        "Master to connect to.",
        [](const Option<string>& value) -> Option<Error> {
          if (value.isNone()) {
            return Error("Missing --master");
          }

          return None();
        });

    add(&build_dir,
        "build_dir",
        "The build directory of Mesos. If set, the framework will assume\n"
        "that the executor, framework, and agent(s) all live on the same\n"
        "machine.");

    add(&executor_uri,
        "executor_uri",
        "URI the fetcher should use to get the executor.");

    add(&executor_command,
        "executor_command",
        "The command that should be used to start the executor.\n"
        "This will override the value set by `--build_dir`.");

    add(&checkpoint,
        "checkpoint",
        "Whether this framework should be checkpointed.",
        false);
  }

  Option<string> master;

  // Flags for specifying the executor binary.
  Option<string> build_dir;
  Option<string> executor_uri;
  Option<string> executor_command;

  bool checkpoint;
};


int main(int argc, char** argv)
{
  Flags flags;
  Try<Nothing> load = flags.load("MESOS_", argc, argv);

  if (load.isError()) {
    EXIT(EXIT_FAILURE) << flags.usage(load.error());
  }

  const Resources resources = Resources::parse(
      "cpus:" + stringify(CPUS_PER_EXECUTOR) +
      ";mem:" + stringify(MEM_PER_EXECUTOR)).get();

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_resources()->CopyFrom(resources);
  executor.set_name("Long Lived Executor (C++)");
  executor.set_source("cpp_long_lived_framework");

  // Determine the command to run the executor based on three possibilities:
  //   1) `--executor_command` was set, which overrides the below cases.
  //   2) We are in the Mesos build directory, so the targeted executable
  //      is actually a libtool wrapper script.
  //   3) We have not detected the Mesos build directory, so assume the
  //      executor is in the same directory as the framework.
  string command;

  // Find this executable's directory to locate executor.
  if (flags.executor_command.isSome()) {
    command = flags.executor_command.get();
  } else if (flags.build_dir.isSome()) {
    command = path::join(
        flags.build_dir.get(), "src", "long-lived-executor");
  } else {
    command = path::join(
        os::realpath(Path(argv[0]).dirname()).get(),
        "long-lived-executor");
  }

  executor.mutable_command()->set_value(command);

  // Copy `--executor_uri` into the command.
  if (flags.executor_uri.isSome()) {
    mesos::CommandInfo::URI* uri = executor.mutable_command()->add_uris();
    uri->set_value(flags.executor_uri.get());
    uri->set_executable(true);
  }

  LongLivedScheduler scheduler(executor);

  FrameworkInfo framework;
  framework.set_user(os::user().get());
  framework.set_name("Long Lived Framework (C++)");
  framework.set_checkpoint(flags.checkpoint);

  MesosSchedulerDriver* driver;

  // TODO(josephw): Refactor these into a common set of flags.
  if (os::getenv("MESOS_AUTHENTICATE").isSome()) {
    LOG(INFO) << "Enabling authentication for the framework";

    Option<string> value = os::getenv("DEFAULT_PRINCIPAL");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting authentication principal in the environment";
    }

    Credential credential;
    credential.set_principal(value.get());

    framework.set_principal(value.get());

    value = os::getenv("DEFAULT_SECRET");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting authentication secret in the environment";
    }

    credential.set_secret(value.get());

    driver = new MesosSchedulerDriver(
        &scheduler, framework, flags.master.get(), credential);
  } else {
    framework.set_principal("long-lived-framework-cpp");

    driver = new MesosSchedulerDriver(
        &scheduler, framework, flags.master.get());
  }

  int status = driver->run() == DRIVER_STOPPED ? 0 : 1;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}
