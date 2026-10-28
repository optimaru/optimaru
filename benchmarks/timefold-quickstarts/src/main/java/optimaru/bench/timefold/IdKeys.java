package optimaru.bench.timefold;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;

import ai.timefold.solver.core.api.domain.common.PlanningId;

import com.fasterxml.jackson.databind.BeanDescription;
import com.fasterxml.jackson.databind.DeserializationConfig;
import com.fasterxml.jackson.databind.JavaType;
import com.fasterxml.jackson.databind.KeyDeserializer;
import com.fasterxml.jackson.databind.module.SimpleModule;

/**
 * Map keys that are domain objects (e.g. {@code Map<Product, Duration>}) are
 * written as ids; this builds a placeholder carrying only the id, which
 * {@link Graph#relink} then swaps for the real instance.
 */
final class IdKeys {

    private IdKeys() {
    }

    static SimpleModule module() {
        SimpleModule module = new SimpleModule("optimaru-id-keys");
        module.setKeyDeserializers(new com.fasterxml.jackson.databind.module.SimpleKeyDeserializers() {
            @Override
            public KeyDeserializer findKeyDeserializer(JavaType type, DeserializationConfig config,
                    BeanDescription description) {
                return forType(type);
            }
        });
        return module;
    }

    private static KeyDeserializer forType(JavaType type) {
        {
            Field idField = idField(type.getRawClass());
            if (idField == null) {
                return null;
            }
            return new KeyDeserializer() {
                @Override
                public Object deserializeKey(String key, com.fasterxml.jackson.databind.DeserializationContext context) {
                    try {
                        Constructor<?> constructor = type.getRawClass().getDeclaredConstructor();
                        constructor.setAccessible(true);
                        Object placeholder = constructor.newInstance();
                        idField.set(placeholder, idField.getType() == String.class ? key : Long.valueOf(key));
                        return placeholder;
                    } catch (ReflectiveOperationException e) {
                        throw new IllegalStateException(e);
                    }
                }
            };
        }
    }

    private static Field idField(Class<?> type) {
        for (Class<?> c = type; c != null && c != Object.class; c = c.getSuperclass()) {
            for (Field field : c.getDeclaredFields()) {
                if (field.isAnnotationPresent(PlanningId.class)) {
                    field.setAccessible(true);
                    return field;
                }
            }
        }
        return null;
    }
}
