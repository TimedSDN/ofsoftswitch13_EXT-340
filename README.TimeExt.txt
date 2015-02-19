Overview
========
The OpenFlow time extension (EXT-340) allows the controller to send OpenFlow commands that include an execution time, indicating to the switch *when* the respective command should be performed.

The time extension is defined using the Bundle feature (defined in OpenFlow 1.4). A Bundle Commit message may include the time of execution - in this case we call it a Scheduled Bundle.


Prerequisites
=============
Make sure you are familiar with the Bundle feature (OpenFlow 1.4).
It is assumed that switches' clocks are synchronized. This can be done, for example, using NTP, PTP, or ReversePTP. The time extension does not mandate a specific clock synchronization mechanism.


Demos
=====
This release includes two short Python scripts that demonstrate the usage of the time extension:
bundle_time_demo.py
bundle_time_discard_demo.py


Scheduling tolerance
====================
When a switch receives a Scheduled Commit message, it verifies that the scheduled time is not too far in the past or in the future. This check is determined by two parameters: sched_max_past and sched_max_future.
The controller can configure these two parameters in every switch that supports the time extension.


Using Dpctl to configure the scheduling tolerance
=================================================
Use the bundle feature request to verify that the switch supports the time extension, and to configure the sched_max_future and sched_max_past. Since both parameters are 1 second by default, it is recommended to always configure these parameters before you start using Scheduled Bundles. 

The bundle feature request has the following form:

> ./utilities/dpctl tcp:localhost:6635 bundle-feature -f 2 -P <sched_max_past.seconds> -p <sched_max_past.nanoseconds> -S <sched_max_future.seconds> -s <sched_max_future.nanoseconds>

The option -f 2 means that this is feature request includes the time flag, indicating that the two scheduling tolerance parameters are present.

Example:
The following Dpctl command configures both parameters to 100 seconds.

> ./utilities/dpctl tcp:localhost:6635 bundle-feature -f 2 -P 100 -S 100

The following Dpctl command displays the sched_max_past and sched_max_future parameters that are currently configured at the switch.

> ./utilities/dpctl tcp:localhost:6635 bundle-feature -f 0 

Using Dpctl to send Scheduled Bundles
=====================================
A Scheduled Commit includes the time at which the switch is scheduled to perform the Bundle. A scheduled 

> ./utilities/dpctl tcp:localhost:6635 bundle commit -b <bundle-ID> -f 4 -T <ScheduledTime.Seconds> -N <ScheduledTime.nanoSeconds>

The option -f 4 means that this is a *scheduled* commit.

The ScheduledTime is expressed in UTC format (seconds.nanoseconds). If you want to know the current time on you Linux machine use:
> date '+%s.%N'

Example for a Scheduled Bundle procedure:

> ./utilities/dpctl tcp:localhost:6635 bundle open -b 17
> ./utilities/dpctl tcp:localhost:6635 flow-mod -b 17 table=0,cmd=add ,in_port=2, apply:output=1
> ./utilities/dpctl tcp:localhost:6635 bundle commit -b 17 -f 4 -T 1411648179 -N 123456789

After completing these three steps, Dpctl waits until the switch completes the scheduled bundle and sends an acknowledgement. This may take a while...


Using Dpctl to cancel Scheduled Bundles
=======================================
Once you invoked a Scheduled Bundle, and before it is executed, you can send a Bundle Discard message, which cancels the Scheduled Bundle.
when you send a Scheduled Commit, Dpctl waits until it receives an acknowledgement. Thus it is a good idea to invoke Dpctl in the background (using & at the end of the line), and then you can send a Bundle Discard.

Example:
...
> ./utilities/dpctl tcp:localhost:6635 bundle commit -b 17 -f 4 -T 1411648179 -N 123456789 &
> ./utilities/dpctl tcp:localhost:6635 bundle discard -b 17
