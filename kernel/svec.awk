#
# svec.awk - extract insmod parameters from transfer.ref
#
# usage: svec.awk DEVICE_NAME [transfer_file]
#
# e.g.:
#  $ awk -f svec.awk FMC_SVEC /acc/dsc/tst/cfv-864-cdv28/etc/transfer.ref
#  $ awk -f svec.awk FMC-SVEC /acc/dsc/tst/cfv-864-cdv28/etc/transfer.ref
#
#  produces
#    vmebase1=a0000000 vmebase2=a0000000 vector=0x00
#

BEGIN {
	device_name = ARGV[1]
	delete ARGV[1]
	slot = ""
	csr_base_addr = ""
	a32_base_addr = ""
	vector = ""
}

/^#\+#/ && $6 == device_name  && $4 == "VME" {
	# decode transfer.ref line
	lun = lun "," $7
	slot =  slot "," $20
	csr_base_addr = sprintf("%s,0x%x", csr_base_addr, $20 * 0x80000)
	a32_base_addr = sprintf("%s,0x%s", a32_base_addr, $11)
	vector = vector "," $23
	level = level ",2"		# hack: always level 2
	fw_name = fw_name ",$GOLDEN"	# hack: always golden bitstream
}

END {
	insmod_params = " "

	# take away the first comma in each vector of params
	if (lun)
		insmod_params = insmod_params " lun=" substr(lun, 2)
	if (csr_base_addr)
		insmod_params = insmod_params " vmebase1=" substr(csr_base_addr, 2)
	if (a32_base_addr)
		insmod_params = insmod_params " vmebase2=" substr(a32_base_addr, 2)
	if (vector)
		insmod_params = insmod_params " vector=" substr(vector, 2)
	if (level)
		insmod_params = insmod_params " level=" substr(level, 2)
	if (fw_name)
		insmod_params = insmod_params " fw_name=" substr(fw_name, 2)

	print substr(insmod_params, 3)
}




