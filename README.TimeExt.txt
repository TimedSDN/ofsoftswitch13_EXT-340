Overview
========
The OpenFlow 1.5 Scheduled Bundles (previously known as the time extension, EXT-340), allows the controller to send OpenFlow commands that include an execution time, indicating to the switch *when* the respective command should be performed.

The time extension is defined using the Bundle feature (defined in OpenFlow 1.4). A Bundle Commit message may include the time of execution - in this case we call it a Scheduled Bundle.

The time extension is part of the TimedSDN project (http://tx.technion.ac.il/~dew/TimedSDN.html).


Prerequisites
=============
Make sure you are familiar with the Bundle feature (OpenFlow 1.4).
It is assumed that switches' clocks are synchronized. This can be done, for example, using NTP, PTP, or ReversePTP. The time extension does not mandate a specific clock synchronization mechanism.


Installation
============
The installation is as described in README.md.
In order to install on Ubuntu 14.04, please see the "Installing OpenFlow 1.3 software switch (CPqD)" section of the following link: http://tocai.dia.uniroma3.it/compunet-wiki/index.php/Installing_and_setting_up_OpenFlow_tools .


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

After completing these three steps, Dpctl waits until the switch completes the scheduled bundle and sends an acknowledgement. This may take a while. It is thus recommended to run the commit line in th
e background (using an '&' at the end of the line).


Does the time extension work on Mininet?
========================================
Yes.
You can experiment with the time extension on Mininet. All switches are (by definition) synchronized in Mininet, since they all share the same clock. However, it is not truely possible to perform concurrent updates in Mininet, since Mininet runs on a single machine. Therefore, truely concurrent even
ts can only be tested on a testbed where each switch runs on a different machine.

In order to install on Mininet:
1. Install the switch according to the "Installation" instructions above.
2. Install Mininet:
   >  git clone git://github.com/mininet/mininet
   >  mininet/util/install.sh -n

Now you can run Mininet, and the switches are time-extension-enabled. For example:
Run mininet using:
>  sudo mn --topo single,2 --mac --switch user --controller remote

Use Dpctl as the controller. For example, from the Linux shell (not within Mininet), you can run:
>  sudo dpctl unix:/tmp/s1 stats-flow table=0
>  sudo dpctl unix:/tmp/s1 bundle open -b 17
>  sudo dpctl unix:/tmp/s1 flow-mod -b 17 table=0,cmd=add ,in_port=2, apply:output=1
>  sudo dpctl unix:/tmp/s1 flow-mod -b 17 table=0,cmd=add ,in_port=1, apply:output=2
>  sudo dpctl unix:/tmp/s1 bundle commit -b 17 -f 4 -T 1411648179 -N 123456789


Using Dpctl to cancel Scheduled Bundles
=======================================
Once you invoked a Scheduled Bundle, and before it is executed, you can send a Bundle Discard message, which cancels the Scheduled Bundle.
when you send a Scheduled Commit, Dpctl waits until it receives an acknowledgement. Thus it is a good idea to invoke Dpctl in the background (using & at the end of the line), and then you can send a Bundle Discard.

Example:
...
> ./utilities/dpctl tcp:localhost:6635 bundle commit -b 17 -f 4 -T 1411648179 -N 123456789 &
> ./utilities/dpctl tcp:localhost:6635 bundle discard -b 17


Summary of changes
==================
The current version is based on the OfSoftSwitch EXT-230 prototype (https://github.com/jean2/ofsoftswitch13/tree/jtonsing/ext-230).
The main differences between the current version and the baseline version are in the following files:
- udatapath/datapath.c: the switch's scheduling functionality was added.
- utilities/dpctl.c: Dpctl was modified to support Scheduled Bundles and Bundle Feature messages.
- udatapath/bundle.c: including the switch's ability to process Bundle messages and Bundel Feature messages.
- include/openflow/openflow.h, oflib/ofl-messages-pack.c, oflib/ofl-messages-unpack.c, oflib/ofl-messages-print.c: the new message formats were updated, as well as updates to the existing message formats, as specified in OpenFlow 1.5.
