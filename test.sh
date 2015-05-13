#!/usr/bin/env python

import subprocess
import random
import time

while True:
    time.sleep(random.uniform(1, 9))
    subprocess.check_call('gdbus call -y -d org.verbum.TestExitOnIdle -o /org/verbum/counter -m org.verbum.Counter.Inc && gdbus call -y -d org.verbum.TestExitOnIdle -o /org/verbum/counter -m org.verbum.Counter.Get', shell=True)
