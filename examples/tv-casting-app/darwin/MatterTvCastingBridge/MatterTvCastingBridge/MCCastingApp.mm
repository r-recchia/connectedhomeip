/**
 *
 *    Copyright (c) 2023 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#import "MCCastingApp_Internal.h"

#import "MCCommissionableDataProvider.h"
#import "MCCommonCaseDeviceServerInitParamsProvider.h"
#import "MCDeviceAttestationCredentialsProvider.h"
#import "MCErrorUtils.h"
#import "MCRotatingDeviceIdUniqueIdProvider.h"

#import "core/Types.h"
#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <credentials/attestation_verifier/DeviceAttestationVerifier.h>

#import <Foundation/Foundation.h>

@interface MCCastingApp ()

@property matter::casting::support::AppParameters appParameters;
@property matter::casting::support::MCRotatingDeviceIdUniqueIdProvider uniqueIdProvider;
@property matter::casting::support::MCCommissionableDataProvider * commissionableDataProvider;
@property matter::casting::support::MCDeviceAttestationCredentialsProvider * dacProvider;
@property MCCommonCaseDeviceServerInitParamsProvider * serverInitParamsProvider;

// queue used when calling the client code on completion blocks from any MatterTvCastingBridge API
@property dispatch_queue_t _Nonnull clientQueue;

// queue used to perform all work performed by the MatterTvCastingBridge
@property (atomic) dispatch_queue_t workQueue;

// Client defiend data source used to initialize the MCCommissionableDataProvider and, if needed, update the MCCommissionableDataProvider post initialization. This is necessary for the Commissioner-Generated passcode commissioning feature.
@property (nonatomic, strong) id<MCDataSource> dataSource;

@end

@implementation MCCastingApp

+ (MCCastingApp *)getSharedInstance
{
    static MCCastingApp * instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[self alloc] init];
    });
    return instance;
}

- (dispatch_queue_t)getWorkQueue
{
    return _workQueue;
}

- (dispatch_queue_t)getClientQueue
{
    return _clientQueue;
}

- (NSError *)initializeWithDataSource:(id)dataSource
{
    ChipLogProgress(AppServer, "MCCastingApp.initializeWithDataSource() called");
    // store the data source provided by the client
    _dataSource = dataSource;

    // get the clientQueue
    VerifyOrReturnValue([dataSource clientQueue] != nil, [MCErrorUtils NSErrorFromChipError:CHIP_ERROR_INVALID_ARGUMENT]);
    _clientQueue = [dataSource clientQueue];

    // Initialize cpp Providers
    VerifyOrReturnValue(_uniqueIdProvider.Initialize(dataSource) == CHIP_NO_ERROR, [MCErrorUtils NSErrorFromChipError:CHIP_ERROR_INVALID_ARGUMENT]);

    ChipLogProgress(AppServer, "MCCastingApp.initializeWithDataSource() calling MCCommissionableDataProvider.Initialize()");
    _commissionableDataProvider = new matter::casting::support::MCCommissionableDataProvider();
    VerifyOrReturnValue(_commissionableDataProvider->Initialize(dataSource) == CHIP_NO_ERROR, [MCErrorUtils NSErrorFromChipError:CHIP_ERROR_INVALID_ARGUMENT]);

    _dacProvider = new matter::casting::support::MCDeviceAttestationCredentialsProvider();
    VerifyOrReturnValue(_dacProvider->Initialize(dataSource) == CHIP_NO_ERROR, [MCErrorUtils NSErrorFromChipError:CHIP_ERROR_INVALID_ARGUMENT]);

    _serverInitParamsProvider = new MCCommonCaseDeviceServerInitParamsProvider();

    // Create cpp AppParameters
    // TODO: Properly validate revocation!
    chip::Credentials::DeviceAttestationRevocationDelegate * kDeviceAttestationRevocationNotChecked = nullptr;
    VerifyOrReturnValue(_appParameters.Create(&_uniqueIdProvider, _commissionableDataProvider, _dacProvider,
                            GetDefaultDACVerifier(chip::Credentials::GetTestAttestationTrustStore(), kDeviceAttestationRevocationNotChecked), _serverInitParamsProvider)
            == CHIP_NO_ERROR,
        [MCErrorUtils NSErrorFromChipError:CHIP_ERROR_INVALID_ARGUMENT]);

    ChipLogProgress(AppServer, "MCCastingApp.initializeWithDataSource() calling cpp CastingApp::Initialize()");
    // Initialize cpp CastingApp
    VerifyOrReturnValue(matter::casting::core::CastingApp::GetInstance()->Initialize(_appParameters) == CHIP_NO_ERROR,
        [MCErrorUtils NSErrorFromChipError:CHIP_ERROR_INCORRECT_STATE]);

    // Get and store the CHIP Work queue
    _workQueue = chip::DeviceLayer::PlatformMgrImpl().GetWorkQueue();

    return [MCErrorUtils NSErrorFromChipError:CHIP_NO_ERROR];
}

- (NSError *)updateCommissionableDataProvider
{
    ChipLogProgress(AppServer, "MCCastingApp.UpdateCommissionableDataProvider() called");

    _commissionableDataProvider = new matter::casting::support::MCCommissionableDataProvider();
    VerifyOrReturnValue(_commissionableDataProvider->Initialize(_dataSource) == CHIP_NO_ERROR,
        [MCErrorUtils NSErrorFromChipError:CHIP_ERROR_INVALID_ARGUMENT]);

    ChipLogProgress(AppServer, "MCCastingApp.initializeWithDataSource() calling cpp CastingApp::UpdateCommissionableDataProvider()");
    // Update cpp CastingApp CommissionableDataProvider
    VerifyOrReturnValue(matter::casting::core::CastingApp::GetInstance()->UpdateCommissionableDataProvider(_commissionableDataProvider) == CHIP_NO_ERROR,
        [MCErrorUtils NSErrorFromChipError:CHIP_ERROR_INCORRECT_STATE]);

    return [MCErrorUtils NSErrorFromChipError:CHIP_NO_ERROR];
}

- (void)startWithCompletionBlock:(void (^)(NSError *))completion
{
    ChipLogProgress(AppServer, "MCCastingApp.startWithCompletionBlock called");
    VerifyOrReturn(_workQueue != nil && _clientQueue != nil, dispatch_async(self->_clientQueue, ^{
        completion([MCErrorUtils NSErrorFromChipError:CHIP_ERROR_INCORRECT_STATE]);
    }));

    dispatch_async(_workQueue, ^{
        __block CHIP_ERROR err = matter::casting::core::CastingApp::GetInstance()->Start();
        dispatch_async(self->_clientQueue, ^{
            completion([MCErrorUtils NSErrorFromChipError:err]);
        });
    });
    __block CHIP_ERROR err = chip::DeviceLayer::PlatformMgrImpl().StartEventLoopTask();
    VerifyOrReturn(err == CHIP_NO_ERROR, dispatch_async(self->_clientQueue, ^{
        completion([MCErrorUtils NSErrorFromChipError:err]);
    }));
}

- (void)stopWithCompletionBlock:(void (^)(NSError *))completion
{
    ChipLogProgress(AppServer, "MCCastingApp.stopWithCompletionBlock called");
    VerifyOrReturn(_workQueue != nil && _clientQueue != nil, dispatch_async(self->_clientQueue, ^{
        completion([MCErrorUtils NSErrorFromChipError:CHIP_ERROR_INCORRECT_STATE]);
    }));

    dispatch_async(_workQueue, ^{
        __block CHIP_ERROR err = matter::casting::core::CastingApp::GetInstance()->Stop();

        dispatch_async(self->_clientQueue, ^{
            completion([MCErrorUtils NSErrorFromChipError:err]);
        });
    });
}

- (bool)isRunning
{
    VerifyOrReturnValue(_workQueue != nil && _clientQueue != nil, false);

    __block bool running = false;
    dispatch_sync(_workQueue, ^{
        running = matter::casting::core::CastingApp::GetInstance()->isRunning();
    });
    return running;
}

- (NSError *)ShutdownAllSubscriptions
{
    ChipLogProgress(AppServer, "MCCastingApp.ShutdownAllSubscriptions called");
    __block CHIP_ERROR err = CHIP_NO_ERROR;
    dispatch_sync(_workQueue, ^{
        err = matter::casting::core::CastingApp::GetInstance()->ShutdownAllSubscriptions();
    });
    return [MCErrorUtils NSErrorFromChipError:err];
}

- (NSError *)ClearCache
{
    ChipLogProgress(AppServer, "MCCastingApp.ClearCache called");
    __block CHIP_ERROR err = CHIP_NO_ERROR;
    dispatch_sync(_workQueue, ^{
        err = matter::casting::core::CastingApp::GetInstance()->ClearCache();
    });
    return [MCErrorUtils NSErrorFromChipError:err];
}

@end
