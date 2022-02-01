// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::container::ComponentIdentity;
use async_trait::async_trait;
use fidl_fuchsia_io::DirectoryProxy;
use fidl_fuchsia_logger::LogSinkRequestStream;
use futures::{channel::mpsc, SinkExt, StreamExt};
use std::{
    collections::{BTreeMap, BTreeSet},
    iter::Extend,
    sync::{Arc, Weak},
};
use thiserror::Error;

const MAX_EVENT_BUS_CAPACITY: usize = 1024;

/// Core archivist internal event router that supports multiple event producers and multiple event
/// consumers.
pub struct EventRouter {
    consumers: BTreeMap<AnyEventType, Vec<Weak<dyn EventConsumer + Send + Sync>>>,
    producers_registered: BTreeSet<AnyEventType>,
    event_sender: mpsc::Sender<Event>,
    event_receiver: mpsc::Receiver<Event>,
}

impl EventRouter {
    /// Creates a new empty event router.
    pub fn new() -> Self {
        let (event_sender, event_receiver) = mpsc::channel(MAX_EVENT_BUS_CAPACITY);
        Self {
            consumers: BTreeMap::new(),
            event_sender,
            event_receiver,
            producers_registered: BTreeSet::new(),
        }
    }

    /// Registers an event producer with the given configuration specifying the types of events the
    /// given producer is allowed to emit.
    pub fn add_producer<T>(&mut self, config: ProducerConfig<'_, T>)
    where
        T: EventProducer,
    {
        let mut events: BTreeSet<_> = config.events.into_iter().map(|e| e.into()).collect();
        events.extend(config.singleton_events.into_iter().map(|e| e.into()));
        self.producers_registered.append(&mut events.clone());
        let dispatcher = Dispatcher::new(events, self.event_sender.clone());
        config.producer.set_dispatcher(dispatcher);
    }

    /// Registers an event consumer with the given configuration specifying the types of events the
    /// given consumer will receive.
    pub fn add_consumer<T: 'static>(&mut self, config: ConsumerConfig<'_, T>)
    where
        T: EventConsumer + Send + Sync,
    {
        let subscriber_weak = Arc::downgrade(config.consumer);
        for event_type in config.events {
            self.consumers.entry(event_type.into()).or_default().push(subscriber_weak.clone());
        }
        for event_type in config.singleton_events {
            self.consumers.entry(event_type.into()).or_default().push(subscriber_weak.clone());
        }
    }

    /// Starts listening for events emitted by the registered producers and dispatching them to
    /// registered consumers.
    ///
    /// First, validates that for every event type that will be dispatched, there exists at least
    /// one consumer. And that for every event that will be consumed, there exists at least one
    /// producer.
    ///
    /// Afterwards, listens to events emitted by producers. When an event arrives it sends it to
    /// all consumers of the event. If the event is singleton, the first consumer that was
    /// registered will get the singleton data and the rest won't.
    pub async fn start(mut self) -> Result<(), RouterError> {
        self.validate_routing()?;
        while let Some(event) = self.event_receiver.next().await {
            let consumers = match self.consumers.get_mut(&event.ty()) {
                Some(consumers) => consumers,
                None => continue,
            };

            let event_type = event.ty();
            let event_without_singleton_data = event.clone();
            let mut event_with_singleton_data =
                if event.is_singleton() { Some(event) } else { None };

            let mut active_consumers = vec![];
            for consumer in consumers.into_iter().filter_map(|c| c.upgrade()) {
                active_consumers.push(Arc::downgrade(&consumer));
                let e = event_with_singleton_data
                    .take()
                    .unwrap_or(event_without_singleton_data.clone());
                consumer.handle(e).await;
            }
            self.consumers.insert(event_type, active_consumers);
        }
        Ok(())
    }

    fn validate_routing(&mut self) -> Result<(), RouterError> {
        for consumed_event in self.consumers.keys() {
            if self.producers_registered.get(&consumed_event).is_none() {
                return Err(RouterError::MissingProducer(consumed_event.clone()));
            }
        }
        for produced_event in &self.producers_registered {
            if self.consumers.get(&produced_event).is_none() {
                return Err(RouterError::MissingConsumer(produced_event.clone()));
            }
        }
        Ok(())
    }
}

/// Allows to emit events of a restricted set of types.
///
/// Event producers will receive a `Dispatcher` instance that will allow them to emit events of
/// restricted set of types.
pub struct Dispatcher {
    allowed_events: BTreeSet<AnyEventType>,
    sender: Option<mpsc::Sender<Event>>,
}

/// Returns a no-op dispatcher.
impl Default for Dispatcher {
    fn default() -> Self {
        Self { allowed_events: BTreeSet::new(), sender: None }
    }
}

impl Dispatcher {
    fn new(allowed_events: BTreeSet<AnyEventType>, sender: mpsc::Sender<Event>) -> Self {
        Self { allowed_events, sender: Some(sender) }
    }

    /// Emits an event. If the event isn't in the restricted set of allowed types, this operation
    /// is a no-op. An error is returned when sending the event into the channel fails.
    pub async fn emit(&mut self, event: Event) -> Result<(), mpsc::SendError> {
        if let Some(sender) = &mut self.sender {
            if self.allowed_events.contains(&event.ty()) {
                sender.send(event).await?;
            }
        }
        Ok(())
    }
}

/// Set of errors that can happen when setting up an event router and executing its dispatching loop.
#[derive(Debug, Error)]
pub enum RouterError {
    #[error("Missing consumer for event type {0:?}")]
    MissingConsumer(AnyEventType),

    #[error("Missing producer for event type {0:?}")]
    MissingProducer(AnyEventType),
}

/// Configuration for an event producer.
pub struct ProducerConfig<'a, T> {
    /// The event producer that will receive a `Dispatcher`
    pub producer: &'a mut T,

    /// The set of events that the `producer` will be allowed to emit.
    pub events: Vec<EventType>,

    /// The set of singleton events that the `producer` will be allowed to emit.
    pub singleton_events: Vec<SingletonEventType>,
}

/// Configuration for an event consumer.
pub struct ConsumerConfig<'a, T> {
    /// The event consumer that will receive events when they are emitted by producers.
    pub consumer: &'a Arc<T>,

    /// The set of event types that the `consumer` will receive.
    pub events: Vec<EventType>,

    /// The set of singleton event types that the `consumer` will receive.
    pub singleton_events: Vec<SingletonEventType>,
}

/// Wrapper for all types of events.
#[derive(Clone, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum AnyEventType {
    General(EventType),
    Singleton(SingletonEventType),
}

/// Event types that don't contain singleton data and can be cloned directly.
#[derive(Clone, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum EventType {
    ComponentStarted,
    ComponentStopped,
    InspectDisconnected,
    LogSinkRequested,
    LogsDisconnected,
}

/// Event types that contain singleton data. When these events are cloned, their singleton data
/// won't be cloned.
#[derive(Clone, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum SingletonEventType {
    DiagnosticsReady,
    LogSinkRequested,
    LogsReady,
}

impl Into<AnyEventType> for EventType {
    fn into(self) -> AnyEventType {
        AnyEventType::General(self)
    }
}

impl Into<AnyEventType> for SingletonEventType {
    fn into(self) -> AnyEventType {
        AnyEventType::Singleton(self)
    }
}

/// Trait implemented by data types which receive events.
#[async_trait]
pub trait EventConsumer {
    /// Event consumers will receive a call on this method when an event they are interested on
    /// happens.
    async fn handle(self: Arc<Self>, event: Event);
}

/// Trait implemented by data types which emit events.
pub trait EventProducer {
    /// Whent registered, event producers will receive a call on this method with the `dispatcher`
    /// they can use to emit events.
    fn set_dispatcher(&mut self, dispatcher: Dispatcher);
}

/// An event that is emitted and consumed.
#[derive(Debug, Clone)]
pub struct Event {
    /// The contents of the event.
    pub payload: EventPayload,
}

impl Event {
    fn is_singleton(&self) -> bool {
        matches!(self.ty(), AnyEventType::Singleton(_))
    }

    fn ty(&self) -> AnyEventType {
        match &self.payload {
            EventPayload::ComponentStarted(_) => EventType::ComponentStarted.into(),
            EventPayload::ComponentStopped(_) => EventType::ComponentStopped.into(),
            EventPayload::LogSinkRequested(_) => SingletonEventType::LogSinkRequested.into(),
            EventPayload::DiagnosticsReady(_) => SingletonEventType::DiagnosticsReady.into(),
        }
    }
}

/// The contents of the event depending on the type of event.
#[derive(Debug, Clone)]
pub enum EventPayload {
    ComponentStarted(ComponentStartedPayload),
    ComponentStopped(ComponentStoppedPayload),
    LogSinkRequested(LogSinkRequestedPayload),
    DiagnosticsReady(DiagnosticsReadyPayload),
}

/// Payload for a started event.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ComponentStartedPayload {
    /// The component that started.
    pub component: ComponentIdentity,
}

/// Payload for a stopped event.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ComponentStoppedPayload {
    /// The component that stopped.
    pub component: ComponentIdentity,
}

/// Payload for a CapabilityReady(diagnostics) event.
#[derive(Debug)]
pub struct DiagnosticsReadyPayload {
    /// The component which diagnostics directory is available.
    pub component: ComponentIdentity,
    /// The `out/diagnostics` directory of the component.
    pub directory: Option<DirectoryProxy>,
}

impl Clone for DiagnosticsReadyPayload {
    fn clone(&self) -> Self {
        Self { component: self.component.clone(), directory: None }
    }
}

/// Payload for a connection to the `LogSink` protocol.
pub struct LogSinkRequestedPayload {
    /// The component that is connecting to `LogSink`.
    pub component: ComponentIdentity,
    /// The stream containing requests made on the `LogSink` channel by the component.
    pub request_stream: Option<LogSinkRequestStream>,
}

impl Clone for LogSinkRequestedPayload {
    fn clone(&self) -> Self {
        Self { component: self.component.clone(), request_stream: None }
    }
}

impl std::fmt::Debug for LogSinkRequestedPayload {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("LogSinkRequestedPayload").field("component", &self.component).finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::events::types::ComponentIdentifier;
    use assert_matches::assert_matches;
    use fidl_fuchsia_logger::LogSinkMarker;
    use fuchsia_async as fasync;
    use futures::{lock::Mutex, FutureExt};
    use lazy_static::lazy_static;

    const TEST_URL: &'static str = "NO-OP URL";
    lazy_static! {
        static ref IDENTITY: ComponentIdentity = ComponentIdentity::from_identifier_and_url(
            ComponentIdentifier::parse_from_moniker("./a/b").unwrap(),
            TEST_URL
        );
    }

    #[derive(Default)]
    struct TestEventProducer {
        dispatcher: Dispatcher,
    }

    impl EventProducer for TestEventProducer {
        fn set_dispatcher(&mut self, dispatcher: Dispatcher) {
            self.dispatcher = dispatcher;
        }
    }

    struct TestEventConsumer {
        event_sender: Mutex<mpsc::Sender<Event>>,
    }

    impl TestEventConsumer {
        fn new() -> (mpsc::Receiver<Event>, Arc<Self>) {
            let (event_sender, event_receiver) = mpsc::channel(10);
            (event_receiver, Arc::new(Self { event_sender: Mutex::new(event_sender) }))
        }
    }

    #[async_trait]
    impl EventConsumer for TestEventConsumer {
        async fn handle(self: Arc<Self>, event: Event) {
            self.event_sender.lock().await.send(event).await.unwrap();
        }
    }

    #[fuchsia::test]
    async fn invalid_routing() {
        let mut producer = TestEventProducer::default();
        let (_receiver, consumer) = TestEventConsumer::new();
        let mut router = EventRouter::new();
        router.add_producer(ProducerConfig {
            producer: &mut producer,
            events: vec![EventType::ComponentStarted],
            singleton_events: vec![],
        });
        router.add_consumer(ConsumerConfig {
            consumer: &consumer,
            events: vec![EventType::ComponentStopped],
            singleton_events: vec![],
        });

        let result = router.start().await;
        assert_matches!(
            result,
            Err(RouterError::MissingConsumer(AnyEventType::General(EventType::ComponentStarted)))
                | Err(RouterError::MissingProducer(AnyEventType::General(
                    EventType::ComponentStopped
                )))
        );

        let mut producer = TestEventProducer::default();
        let (_receiver, consumer) = TestEventConsumer::new();
        let mut router = EventRouter::new();
        router.add_producer(ProducerConfig {
            producer: &mut producer,
            events: vec![],
            singleton_events: vec![SingletonEventType::DiagnosticsReady],
        });
        router.add_consumer(ConsumerConfig {
            consumer: &consumer,
            events: vec![],
            singleton_events: vec![SingletonEventType::LogSinkRequested],
        });

        let result = router.start().await;
        assert_matches!(
            result,
            Err(RouterError::MissingConsumer(AnyEventType::Singleton(
                SingletonEventType::DiagnosticsReady
            ))) | Err(RouterError::MissingProducer(AnyEventType::Singleton(
                SingletonEventType::LogSinkRequested
            )))
        );
    }

    #[fuchsia::test]
    async fn singleton_event_subscription() {
        let mut producer = TestEventProducer::default();
        let (mut first_receiver, first_consumer) = TestEventConsumer::new();
        let (mut second_receiver, second_consumer) = TestEventConsumer::new();
        let mut router = EventRouter::new();
        router.add_producer(ProducerConfig {
            producer: &mut producer,
            events: vec![],
            singleton_events: vec![SingletonEventType::LogSinkRequested],
        });
        router.add_consumer(ConsumerConfig {
            consumer: &first_consumer,
            events: vec![],
            singleton_events: vec![SingletonEventType::LogSinkRequested],
        });
        router.add_consumer(ConsumerConfig {
            consumer: &second_consumer,
            events: vec![],
            singleton_events: vec![SingletonEventType::LogSinkRequested],
        });

        let _router_task = fasync::Task::spawn(async move { router.start().await.unwrap() });

        // Emit an event
        let (_, request_stream) =
            fidl::endpoints::create_request_stream::<LogSinkMarker>().unwrap();
        producer
            .dispatcher
            .emit(Event {
                payload: EventPayload::LogSinkRequested(LogSinkRequestedPayload {
                    component: IDENTITY.clone(),
                    request_stream: Some(request_stream),
                }),
            })
            .await
            .unwrap();

        // The first consumer that was registered must receive the request stream. The second one
        // must receive no payload, but still receive the event.
        let first_event = first_receiver.next().await.unwrap();
        assert_matches!(first_event, Event { payload: EventPayload::LogSinkRequested(payload) } => {
            assert_eq!(payload.component, *IDENTITY);
            assert!(payload.request_stream.is_some());
        });
        let second_event = second_receiver.next().await.unwrap();
        assert_matches!(second_event, Event { payload: EventPayload::LogSinkRequested(payload) } => {
            assert_eq!(payload.component, *IDENTITY);
            assert!(payload.request_stream.is_none());
        });
    }

    #[fuchsia::test]
    async fn regular_event_subscription() {
        let mut producer = TestEventProducer::default();
        let (mut first_receiver, first_consumer) = TestEventConsumer::new();
        let (mut second_receiver, second_consumer) = TestEventConsumer::new();
        let mut router = EventRouter::new();
        router.add_producer(ProducerConfig {
            producer: &mut producer,
            events: vec![EventType::ComponentStarted],
            singleton_events: vec![],
        });
        router.add_consumer(ConsumerConfig {
            consumer: &first_consumer,
            events: vec![EventType::ComponentStarted],
            singleton_events: vec![],
        });
        router.add_consumer(ConsumerConfig {
            consumer: &second_consumer,
            events: vec![EventType::ComponentStarted],
            singleton_events: vec![],
        });
        let _router_task = fasync::Task::spawn(async move { router.start().await.unwrap() });

        // Emit an event
        producer
            .dispatcher
            .emit(Event {
                payload: EventPayload::ComponentStarted(ComponentStartedPayload {
                    component: IDENTITY.clone(),
                }),
            })
            .await
            .unwrap();

        // Both consumers receive the exact same event.
        let first_event = first_receiver.next().await.unwrap();
        assert_matches!(first_event, Event { payload: EventPayload::ComponentStarted(payload) } => {
            assert_eq!(payload, ComponentStartedPayload { component: IDENTITY.clone() });
        });
        let second_event = second_receiver.next().await.unwrap();
        assert_matches!(
            second_event,
            Event { payload: EventPayload::ComponentStarted(payload) } => {
                assert_eq!(payload, ComponentStartedPayload { component: IDENTITY.clone() });
            }
        );
    }

    #[fuchsia::test]
    async fn consumers_cleanup() {
        let mut producer = TestEventProducer::default();
        let (mut first_receiver, first_consumer) = TestEventConsumer::new();
        let (mut second_receiver, second_consumer) = TestEventConsumer::new();
        let (mut third_receiver, third_consumer) = TestEventConsumer::new();
        let mut router = EventRouter::new();
        router.add_producer(ProducerConfig {
            producer: &mut producer,
            events: vec![EventType::ComponentStarted],
            singleton_events: vec![],
        });
        router.add_consumer(ConsumerConfig {
            consumer: &first_consumer,
            events: vec![EventType::ComponentStarted],
            singleton_events: vec![],
        });
        router.add_consumer(ConsumerConfig {
            consumer: &second_consumer,
            events: vec![EventType::ComponentStarted],
            singleton_events: vec![],
        });
        router.add_consumer(ConsumerConfig {
            consumer: &third_consumer,
            events: vec![EventType::ComponentStarted],
            singleton_events: vec![],
        });

        drop(first_consumer);
        drop(third_consumer);

        let _router_task = fasync::Task::spawn(async move { router.start().await.unwrap() });

        // Emit an event
        producer
            .dispatcher
            .emit(Event {
                payload: EventPayload::ComponentStarted(ComponentStartedPayload {
                    component: IDENTITY.clone(),
                }),
            })
            .await
            .unwrap();

        // We see the event only in the receiver which consumer wasn't dropped.
        let event = second_receiver.next().await.unwrap();
        assert_matches!(event.payload, EventPayload::ComponentStarted(_));
        assert!(first_receiver.next().now_or_never().unwrap().is_none());
        assert!(third_receiver.next().now_or_never().unwrap().is_none());

        // We see additional events in the second receiver which remains alive.
        producer
            .dispatcher
            .emit(Event {
                payload: EventPayload::ComponentStarted(ComponentStartedPayload {
                    component: IDENTITY.clone(),
                }),
            })
            .await
            .unwrap();
        let event = second_receiver.next().await.unwrap();
        assert_matches!(event.payload, EventPayload::ComponentStarted(_));
        assert!(first_receiver.next().now_or_never().unwrap().is_none());
        assert!(third_receiver.next().now_or_never().unwrap().is_none());
    }
}
