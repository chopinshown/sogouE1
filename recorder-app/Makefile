.PHONY: build package deploy factory test-rtp
build:
	sh build.sh
package: build
	python3 package.py
deploy: package
	python3 deploy.py
test-rtp:
	sh test-rtp.sh
factory:
	python3 deploy.py --factory
