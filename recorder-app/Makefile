.PHONY: build package deploy factory test-rtp test-noise
build:
	sh build.sh
package: build
	python3 package.py
deploy: package
	python3 deploy.py
test-rtp:
	sh test-rtp.sh
test-noise:
	sh test-noise.sh
factory:
	python3 deploy.py --factory
