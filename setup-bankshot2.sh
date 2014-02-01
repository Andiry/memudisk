#!/bin/bash

insmod bankshot2.ko rd_nr=1 enable_cache=1 backing_dev_name=/dev/ram0
