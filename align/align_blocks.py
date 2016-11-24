from __future__ import division
import subprocess
import json
import sys
import math

# So there's a bug in the aligner that's corrupting the heap part-way through an alignment run.
# Rather than fixing it - since it might be in PS itself - I just run alignment in blocks of <1000 ayah.
fixed_args = sys.argv[1:4] # Passed through to the program
block_args = sys.argv[4:]
block_size = 1000
n_blocks = int(math.ceil(len(block_args) / block_size))

all_results = []

for i in range(n_blocks):
	sys.stderr.write("Block %d/%d\n" % (i + 1, n_blocks))
	args = ["./align"] + fixed_args + block_args[i * block_size:(i + 1) * block_size]
	while True:
		try:
			result = subprocess.check_output(args)
		except subprocess.CalledProcessError, e:
			print("\nCrashed %s" % e)
			continue
		else:
			break
	all_results += json.loads(result)

json.dump(all_results, sys.stdout)
