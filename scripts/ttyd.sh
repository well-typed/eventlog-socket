#!/bin/sh -e

# Save the command.
COMMAND="$1"

# Ask for consent.
while true; do
	echo "Do you wish to run..."
	echo
	echo "${COMMAND}"
	echo
	read -p "...until it fails? [Yes/No]" yn
	case $yn in
	[Yy]*) break ;;
	[Nn]*) exit ;;
	*) echo "Please answer yes or no." ;;
	esac
done

while true; do
	sh -c "${COMMAND}"
	EXIT_CODE=$?
	if [ ${EXIT_CODE} -ne 0 ]; then
		exit ${EXIT_CODE}
	fi
done
